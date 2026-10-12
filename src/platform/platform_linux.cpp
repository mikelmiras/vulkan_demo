// Linux platform services the window backends share: gamepads, file dialogs, opening files, paths, fonts.
#include "platform/platform.h"
#include "platform/platform_linux.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"

extern char** environ;

namespace platform {

namespace {

// ---------------------------------------------------------------------------------------------
// Gamepads
// ---------------------------------------------------------------------------------------------
struct Pad {
    int js = -1;            // /dev/input/jsN: input
    int event = -1;         // its /dev/input/eventN: force feedback (-1: none, or no permission)
    int effect = -1;        // the rumble effect uploaded there
    std::string node;       // "js0"
    double nextTry = 0.0;   // (none open: when to look again)
};
Pad g_pad;
bool g_padActive = false;   // (between gamepadOpen and gamepadClose)

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool hasBit(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1ul;
}

// The pad's evdev node (for force feedback): /sys/class/input/jsN/device/eventM
void openEventNode(Pad& p) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/input/" + p.node + "/device", ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("event", 0) != 0) continue;
        int fd = open(("/dev/input/" + name).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return;
        unsigned long ff[(FF_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))] = {};
        if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) < 0 || !hasBit(ff, FF_RUMBLE)) {
            close(fd);
            return;
        }
        p.event = fd;
        return;
    }
}

void closePad(Pad& p) {
    if (p.event >= 0) close(p.event);
    if (p.js >= 0) close(p.js);
    p = Pad{};
    p.nextTry = now() + 2.0;
}

void tryOpenPad(Pad& p) {
    if (now() < p.nextTry) return;
    p.nextTry = now() + 2.0;   // (plugged in later: found within two seconds)
    for (int i = 0; i < 4; ++i) {
        std::string node = "js" + std::to_string(i);
        int fd = open(("/dev/input/" + node).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        p.js = fd;
        p.node = node;
        openEventNode(p);
        return;
    }
}

// Linux joystick API (xpad layout: Xbox and most other pads) -> ImGui gamepad keys
void readPad(Pad& p) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto stick = [&](ImGuiKey negative, ImGuiKey positive, int raw) {
        float v = raw / 32767.f, dead = 0.15f;
        float n = std::clamp((-v - dead) / (1.f - dead), 0.f, 1.f), q = std::clamp((v - dead) / (1.f - dead), 0.f, 1.f);
        io.AddKeyAnalogEvent(negative, n > 0.1f, n);
        io.AddKeyAnalogEvent(positive, q > 0.1f, q);
    };
    js_event ev;
    ssize_t got;
    while ((got = read(p.js, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        int type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON) {
            static const ImGuiKey buttons[] = {ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft,
                                               ImGuiKey_GamepadFaceUp, ImGuiKey_GamepadL1, ImGuiKey_GamepadR1,
                                               ImGuiKey_GamepadBack, ImGuiKey_GamepadStart, ImGuiKey_None,
                                               ImGuiKey_GamepadL3, ImGuiKey_GamepadR3};
            if (ev.number < sizeof(buttons) / sizeof(buttons[0]) && buttons[ev.number] != ImGuiKey_None)
                io.AddKeyEvent(buttons[ev.number], ev.value != 0);
        } else if (type == JS_EVENT_AXIS) {
            switch (ev.number) {
                case 0: stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ev.value); break;
                case 1: stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ev.value); break;
                case 3: stick(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, ev.value); break;
                case 4: stick(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, ev.value); break;
                case 6: stick(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ev.value); break;
                case 7: stick(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ev.value); break;
                case 2:
                case 5: {   // triggers: -32767 released .. 32767 pulled
                    float t = (ev.value + 32767) / 65534.f;
                    io.AddKeyAnalogEvent(ev.number == 2 ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2, t > 0.1f, t);
                    break;
                }
            }
        }
    }
    if (got < 0 && errno == ENODEV) closePad(p);   // unplugged
}

bool writeFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream f(file);
    f << text;
    f.flush();
    return (bool)f;
}

// ---------------------------------------------------------------------------------------------
// Dialogs: no toolkit here, so the desktop's dialog helper if one is installed
// ---------------------------------------------------------------------------------------------
std::string runDialog(const std::string* cmds, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const std::string& cmd = cmds[i];
        std::string tool = cmd.substr(0, cmd.find(' '));
        if (std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) != 0) continue;
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) continue;
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;   // empty when cancelled
    }
    return {};
}

}  // namespace

namespace linux_shared {

void gamepadOpen() {
    g_padActive = true;
    g_pad.nextTry = 0.0;
    tryOpenPad(g_pad);
}

void gamepadPoll() {
    if (!g_padActive) return;
    if (g_pad.js < 0) tryOpenPad(g_pad);
    if (g_pad.js >= 0) readPad(g_pad);
}

void gamepadClose() {
    gamepadRumble(0.f, 0.f, 0.f);
    if (g_pad.js >= 0) closePad(g_pad);
    g_padActive = false;
}

}  // namespace linux_shared

// Force feedback through the pad's evdev node: one FF_RUMBLE effect, replaced and played
bool gamepadRumble(float low, float high, float seconds) {
    Pad& p = g_pad;
    if (p.event < 0) return false;
    ff_effect e{};
    e.type = FF_RUMBLE;
    e.id = (int16_t)p.effect;   // (-1: a new one)
    e.u.rumble.strong_magnitude = (uint16_t)(std::clamp(low, 0.f, 1.f) * 0xffff);
    e.u.rumble.weak_magnitude = (uint16_t)(std::clamp(high, 0.f, 1.f) * 0xffff);
    e.replay.length = (uint16_t)std::clamp(seconds * 1000.f, 0.f, 65535.f);
    if (seconds <= 0.f || (low <= 0.f && high <= 0.f)) {   // stop
        if (p.effect < 0) return true;
        input_event stop{};
        stop.type = EV_FF;
        stop.code = (uint16_t)p.effect;
        stop.value = 0;
        return write(p.event, &stop, sizeof(stop)) == (ssize_t)sizeof(stop);
    }
    if (ioctl(p.event, EVIOCSFF, &e) < 0) return false;
    p.effect = e.id;
    input_event play{};
    play.type = EV_FF;
    play.code = (uint16_t)e.id;
    play.value = 1;
    return write(p.event, &play, sizeof(play)) == (ssize_t)sizeof(play);
}

// The pad's LEDs in sysfs (hid-playstation: one "<name>:rgb:indicator" multicolour LED; hid-sony: ":red",
// ":green", ":blue"), under its HID device. Usually only root may write them (a udev rule can allow it).
bool setGamepadLight(float r, float g, float b) {
    if (g_pad.js < 0) return false;
    std::error_code ec;
    std::filesystem::path leds = std::filesystem::path("/sys/class/input") / g_pad.node / "device" / "device" / "leds";
    int R = (int)(std::clamp(r, 0.f, 1.f) * 255.f + 0.5f), G = (int)(std::clamp(g, 0.f, 1.f) * 255.f + 0.5f),
        B = (int)(std::clamp(b, 0.f, 1.f) * 255.f + 0.5f);
    bool any = false, ok = true;
    for (const auto& entry : std::filesystem::directory_iterator(leds, ec)) {
        std::string name = entry.path().filename().string();
        auto endsWith = [&](const char* s) {
            size_t n = std::strlen(s);
            return name.size() >= n && name.compare(name.size() - n, n, s) == 0;
        };
        if (endsWith(":rgb:indicator")) {
            any = true;
            ok = writeFile(entry.path() / "multi_intensity", std::to_string(R) + " " + std::to_string(G) + " " +
                                                               std::to_string(B)) &&
                 writeFile(entry.path() / "brightness", "255") && ok;
        } else if (endsWith(":red") || endsWith(":green") || endsWith(":blue")) {
            any = true;
            int v = endsWith(":red") ? R : endsWith(":green") ? G : B;
            ok = writeFile(entry.path() / "brightness", std::to_string(v)) && ok;
        }
    }
    return any && ok;
}

// ---------------------------------------------------------------------------------------------
// The window: whichever backend made it
// ---------------------------------------------------------------------------------------------
namespace {
bool g_wayland = false;
}

bool createWindow(const char* title, uint32_t width, uint32_t height) {
#ifdef VKWORLD_WAYLAND
    if (wl::wanted() && wl::createWindow(title, width, height)) {
        g_wayland = true;
        return true;
    }
#endif
    g_wayland = false;
    return x11::createWindow(title, width, height);
}

#ifdef VKWORLD_WAYLAND
#define VKWORLD_ON_WINDOW(call) (g_wayland ? wl::call : x11::call)
#else
#define VKWORLD_ON_WINDOW(call) (x11::call)
#endif

void destroyWindow() { VKWORLD_ON_WINDOW(destroyWindow()); }
void pollEvents() { VKWORLD_ON_WINDOW(pollEvents()); }
bool closeRequested() { return VKWORLD_ON_WINDOW(closeRequested()); }
void windowSize(uint32_t& width, uint32_t& height) { VKWORLD_ON_WINDOW(windowSize(width, height)); }
void setCursorCaptured(bool captured) { VKWORLD_ON_WINDOW(setCursorCaptured(captured)); }
bool cursorCaptured() { return VKWORLD_ON_WINDOW(cursorCaptured()); }
void takeMouseDelta(float& dx, float& dy) { VKWORLD_ON_WINDOW(takeMouseDelta(dx, dy)); }
void setFullscreen(bool fullscreen) { VKWORLD_ON_WINDOW(setFullscreen(fullscreen)); }
bool isFullscreen() { return VKWORLD_ON_WINDOW(isFullscreen()); }
void setWindowIcon(const uint8_t* rgba, uint32_t width, uint32_t height) { VKWORLD_ON_WINDOW(setWindowIcon(rgba, width, height)); }
float dpiScale() { return VKWORLD_ON_WINDOW(dpiScale()); }
const char* vulkanSurfaceExtension() { return VKWORLD_ON_WINDOW(vulkanSurfaceExtension()); }
VkSurfaceKHR createVulkanSurface(VkInstance instance) { return VKWORLD_ON_WINDOW(createVulkanSurface(instance)); }
void imguiInit() { VKWORLD_ON_WINDOW(imguiInit()); }
void imguiNewFrame(float deltaTime) { VKWORLD_ON_WINDOW(imguiNewFrame(deltaTime)); }
void imguiShutdown() { VKWORLD_ON_WINDOW(imguiShutdown()); }

bool setExecutableIcon(const std::filesystem::path&, const std::string&, std::string& error) {
    error = "executables have no icons here (the desktop entry has it)";
    return false;
}

std::filesystem::path openFileDialog(const char* title, const char* extension) {
    std::string t = title, patterns;
    for (char& c : t) if (c == '\'') c = ' ';
    for (size_t start = 0, end; start <= std::strlen(extension); start = end + 1) {   // "obj,glb" -> "*.obj *.glb"
        end = std::string(extension).find(',', start);
        if (end == std::string::npos) end = std::strlen(extension);
        std::string e = std::string(extension).substr(start, end - start);
        bool safe = !e.empty() && e.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == std::string::npos;
        if (safe) patterns += (patterns.empty() ? "*." : " *.") + e;
    }
    const std::string cmds[] = {
        "zenity --file-selection --title='" + t + "' --file-filter='" + patterns + "' --file-filter='*' 2>/dev/null",
        "kdialog --getopenfilename . '" + patterns + "' --title '" + t + "' 2>/dev/null"};
    return runDialog(cmds, 2);
}

std::filesystem::path openFolderDialog(const char* title) {
    std::string t = title;
    for (char& c : t) if (c == '\'') c = ' ';
    const std::string cmds[] = {"zenity --file-selection --directory --title='" + t + "' 2>/dev/null",
                                "kdialog --getexistingdirectory . --title '" + t + "' 2>/dev/null"};
    return runDialog(cmds, 2);
}

static const char* const kOpener = "xdg-open";

bool openWithSystem(const std::filesystem::path& file) {
    std::string path = file.u8string();
    char* argv[] = {const_cast<char*>(kOpener), const_cast<char*>(path.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, kOpener, nullptr, nullptr, argv, environ) != 0) return false;
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();   // (reaped when it's done)
    return true;
}

std::filesystem::path executablePath() {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : exe;
}

std::filesystem::path executableDir() {
    std::filesystem::path exe = executablePath();
    return exe.empty() ? std::filesystem::current_path() : exe.parent_path();
}

std::vector<std::filesystem::path> uiFontCandidates() {
    return {"/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"};
}

}  // namespace platform
