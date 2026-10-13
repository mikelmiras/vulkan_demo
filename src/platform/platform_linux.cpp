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
    GamepadState state;     // what it has down now, its kind
    uint16_t buttons[KEY_MAX - BTN_MISC + 1] = {};   // each joystick button's evdev code (JSIOCGBTNMAP)
    uint8_t axes[ABS_CNT] = {};                      // ... each axis's
};
Pad g_pads[4];              // (a pad keeps its slot while it's plugged in)
float g_fed[kGamepadKeys] = {};   // what Dear ImGui was last told (every pad together)
double g_nextScan = 0.0;    // (when to look for pads plugged in)
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
}

// Pads plugged in since the last look (every 2 s): each into a free slot, with its name, kind and code maps
void scanPads() {
    if (now() < g_nextScan) return;
    g_nextScan = now() + 2.0;
    for (int i = 0; i < 8; ++i) {
        std::string node = "js" + std::to_string(i);
        bool open_ = false;
        for (const Pad& p : g_pads) open_ = open_ || p.node == node;
        if (open_) continue;
        Pad* slot = nullptr;
        for (Pad& p : g_pads)
            if (p.js < 0 && !slot) slot = &p;
        if (!slot) return;
        int fd = open(("/dev/input/" + node).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        Pad& p = *slot;
        p.js = fd;
        p.node = node;
        char name[128] = {};
        if (ioctl(fd, JSIOCGNAME(sizeof(name) - 1), name) < 0) name[0] = 0;
        unsigned vendor = 0;   // (USB vendor id: whose pad it is)
        std::ifstream v("/sys/class/input/" + node + "/device/id/vendor");
        v >> std::hex >> vendor;
        p.state = GamepadState{};
        p.state.connected = true;
        p.state.name = name;
        p.state.type = padTypeOf((uint16_t)vendor, name);
        if (ioctl(fd, JSIOCGBTNMAP, p.buttons) < 0) {   // (an old kernel: xpad's order)
            const uint16_t xpad[] = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e};
            std::memcpy(p.buttons, xpad, sizeof(xpad));
        }
        if (ioctl(fd, JSIOCGAXMAP, p.axes) < 0) {
            const uint8_t xpad[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11};
            std::memcpy(p.axes, xpad, sizeof(xpad));
        }
        openEventNode(p);
    }
}

// Linux joystick API events -> the pad's state (ImGui's gamepad keys, by the codes the driver gave)
void readPad(Pad& p) {
    auto stick = [&](ImGuiKey negative, ImGuiKey positive, int raw) {
        float v = raw / 32767.f, dead = 0.15f;
        p.state.set(negative, std::clamp((-v - dead) / (1.f - dead), 0.f, 1.f));
        p.state.set(positive, std::clamp((v - dead) / (1.f - dead), 0.f, 1.f));
    };
    js_event ev;
    ssize_t got;
    while ((got = read(p.js, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        int type = ev.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON && ev.number < sizeof(p.buttons) / sizeof(p.buttons[0])) {
            p.state.set(linuxPadButton(p.buttons[ev.number], p.state.type), ev.value ? 1.f : 0.f);
        } else if (type == JS_EVENT_AXIS && ev.number < sizeof(p.axes)) {
            ImGuiKey negative, positive, trigger;
            if (!linuxPadAxis(p.axes[ev.number], negative, positive, trigger)) continue;
            if (trigger != ImGuiKey_None) p.state.set(trigger, (ev.value + 32767) / 65534.f);   // (-32767 released .. 32767 pulled)
            else stick(negative, positive, ev.value);
        }
    }
    if (got < 0 && errno == ENODEV) closePad(p);   // unplugged
}

// Dear ImGui sees every pad together (menus answer any of them): each control the most any pad has it
void feedImGui() {
    ImGuiIO& io = ImGui::GetIO();
    bool any = false;
    for (const Pad& p : g_pads) any = any || p.js >= 0;
    if (any) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    for (int k = 0; k < kGamepadKeys; ++k) {
        float v = 0.f;
        for (const Pad& p : g_pads)
            if (p.js >= 0) v = std::max(v, p.state.values[k]);
        if (v == g_fed[k]) continue;
        g_fed[k] = v;
        io.AddKeyAnalogEvent((ImGuiKey)(ImGuiKey_GamepadStart + k), v > 0.1f, v);
    }
}

Pad* padAt(int pad) {   // -1: the first connected
    if (pad >= 0) return pad < 4 && g_pads[pad].js >= 0 ? &g_pads[pad] : nullptr;
    for (Pad& p : g_pads)
        if (p.js >= 0) return &p;
    return nullptr;
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
    g_nextScan = 0.0;
    scanPads();
}

void gamepadPoll() {
    if (!g_padActive) return;
    scanPads();
    for (Pad& p : g_pads)
        if (p.js >= 0) readPad(p);
    feedImGui();
}

void gamepadClose() {
    for (int i = 0; i < 4; ++i) {
        gamepadRumble(i, 0.f, 0.f, 0.f);
        if (g_pads[i].js >= 0) closePad(g_pads[i]);
    }
    g_padActive = false;
}

}  // namespace linux_shared

std::vector<GamepadState> gamepads() {
    std::vector<GamepadState> out;
    for (const Pad& p : g_pads) out.push_back(p.js >= 0 ? p.state : GamepadState{});
    return out;
}

bool gamepadRumble(float low, float high, float seconds) { return gamepadRumble(-1, low, high, seconds); }

// Force feedback through the pad's evdev node: one FF_RUMBLE effect, replaced and played
bool gamepadRumble(int pad, float low, float high, float seconds) {
    Pad* found = padAt(pad);
    if (!found || found->event < 0) return false;
    Pad& p = *found;
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
    Pad* pad = padAt(-1);
    if (!pad) return false;
    std::error_code ec;
    std::filesystem::path leds = std::filesystem::path("/sys/class/input") / pad->node / "device" / "device" / "leds";
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
bool waitEvents(double seconds) { return VKWORLD_ON_WINDOW(waitEvents(seconds)); }
bool windowFocused() { return VKWORLD_ON_WINDOW(windowFocused()); }
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

// posix_spawnp with stdout and stderr into one pipe
bool startProgram(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    return posix_spawnp(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), environ) == 0;
}

bool runProgram(const std::vector<std::string>& args, std::string& output, int& exitCode) {
    output.clear();
    exitCode = -1;
    if (args.empty()) return false;
    int out[2];
    if (pipe(out) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, out[1], 2);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (spawned != 0) {
        close(out[0]);
        return false;
    }
    char buf[4096];
    for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) > 0;) output.append(buf, (size_t)n);
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 127 && output.empty());   // (127: no such program)
}

// curl, else wget, run directly (no shell: the URL is one argument) with their output read through a pipe; what's
// POSTed goes through a temporary file
static bool httpRequest(const std::string& url, const std::string* data, const std::string& contentType, std::string& body,
                        std::string& error, int timeoutSeconds) {
    body.clear();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        error = "not an http(s) URL: " + url;
        return false;
    }
    std::string timeout = std::to_string(std::max(timeoutSeconds, 1)), posted;
    if (data) {
        char name[] = "/tmp/vkworld-post-XXXXXX";
        int fd = mkstemp(name);
        if (fd < 0 || write(fd, data->data(), data->size()) != (ssize_t)data->size()) {
            if (fd >= 0) close(fd);
            error = "cannot write a temporary file";
            return false;
        }
        close(fd);
        posted = name;
    }
    std::vector<std::vector<std::string>> tools = {
        {"curl", "-fsSL", "--max-time", timeout},
        {"wget", "-q", "-O", "-", "--timeout=" + timeout}};
    if (data) {
        tools[0].insert(tools[0].end(), {"-X", "POST", "-H", "Content-Type: " + contentType, "--data-binary", "@" + posted});
        tools[1].insert(tools[1].end(), {"--header=Content-Type: " + contentType, "--post-file=" + posted});
    }
    for (auto& t : tools) t.insert(t.end(), {"--", url});
    struct Remove {
        std::string file;
        ~Remove() { if (!file.empty()) unlink(file.c_str()); }
    } remove{posted};
    for (const auto& args : tools) {
        int out[2];
        if (pipe(out) != 0) break;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        int spawned = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        if (spawned != 0) {   // (not installed: the next one)
            close(out[0]);
            continue;
        }
        char buf[4096];
        for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) > 0;) body.append(buf, (size_t)n);
        close(out[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) continue;   // (posix_spawnp found no such program)
        error = args[0] + " failed for " + url + " (exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
        body.clear();
        return false;
    }
    error = "neither curl nor wget is installed";
    return false;
}

bool httpGet(const std::string& url, std::string& body, std::string& error, int timeoutSeconds) {
    return httpRequest(url, nullptr, "", body, error, timeoutSeconds);
}

bool httpPost(const std::string& url, const std::string& data, const std::string& contentType, std::string& body,
              std::string& error, int timeoutSeconds) {
    return httpRequest(url, &data, contentType, body, error, timeoutSeconds);
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
