// Linux platform layer: X11 / XWayland window via XCB.
#define VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "platform/platform.h"

#include <fcntl.h>
#include <linux/joystick.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "imgui.h"
#include "platform/imgui_impl_xcb.h"

namespace platform {

namespace {

xcb_connection_t* g_conn = nullptr;
xcb_window_t g_window = 0, g_root = 0;
xcb_atom_t g_wmDelete = 0;
uint32_t g_width = 0, g_height = 0;
bool g_closeRequested = false;
bool g_imguiReady = false;
bool g_captured = false, g_fullscreen = false;
float g_mouseDx = 0.f, g_mouseDy = 0.f;
xcb_cursor_t g_invisibleCursor = 0;
int g_joystick = -1;   // /dev/input/js0, if there is one

xcb_atom_t internAtom(const char* name) {
    auto cookie = xcb_intern_atom(g_conn, 0, (uint16_t)std::strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(g_conn, cookie, nullptr);
    xcb_atom_t atom = reply ? reply->atom : 0;
    free(reply);
    return atom;
}

void warpToCenter() {
    xcb_warp_pointer(g_conn, XCB_NONE, g_window, 0, 0, 0, 0, (int16_t)(g_width / 2), (int16_t)(g_height / 2));
    xcb_flush(g_conn);
}

// Sees every event before the ImGui backend; returns true to keep it from ImGui
bool onEvent(void*, xcb_generic_event_t* e) {
    switch (e->response_type & 0x7f) {
        case XCB_CONFIGURE_NOTIFY: {
            auto* c = (xcb_configure_notify_event_t*)e;
            g_width = c->width;
            g_height = c->height;
            return false;
        }
        case XCB_CLIENT_MESSAGE:
            if (((xcb_client_message_event_t*)e)->data.data32[0] == g_wmDelete) g_closeRequested = true;
            return false;
        case XCB_MOTION_NOTIFY: {
            if (!g_captured) return false;
            // Captured: movement is measured from the window centre, and the pointer is put back there
            auto* m = (xcb_motion_notify_event_t*)e;
            int dx = m->event_x - (int)(g_width / 2), dy = m->event_y - (int)(g_height / 2);
            if (dx != 0 || dy != 0) {   // (the warp itself produces a motion event at the centre)
                g_mouseDx += (float)dx;
                g_mouseDy += (float)dy;
                warpToCenter();
            }
            return true;
        }
        case XCB_FOCUS_OUT:
            setCursorCaptured(false);
            return false;
    }
    return false;
}

// Linux joystick API (xpad layout: Xbox and most other pads) -> ImGui gamepad keys
void pollJoystick() {
    if (g_joystick < 0 || !g_imguiReady) return;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto stick = [&](ImGuiKey negative, ImGuiKey positive, int raw) {
        float v = raw / 32767.f, dead = 0.15f;
        float n = std::clamp((-v - dead) / (1.f - dead), 0.f, 1.f), p = std::clamp((v - dead) / (1.f - dead), 0.f, 1.f);
        io.AddKeyAnalogEvent(negative, n > 0.1f, n);
        io.AddKeyAnalogEvent(positive, p > 0.1f, p);
    };
    js_event ev;
    while (read(g_joystick, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
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
}

}  // namespace

bool createWindow(const char* title, uint32_t width, uint32_t height) {
    int screenNum = 0;
    g_conn = xcb_connect(nullptr, &screenNum);
    if (xcb_connection_has_error(g_conn)) {
        std::fprintf(stderr, "fatal: cannot connect to X server (is DISPLAY set?)\n");
        return false;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(g_conn));
    for (int i = 0; i < screenNum; ++i) xcb_screen_next(&it);
    xcb_screen_t* screen = it.data;
    g_root = screen->root;

    g_width = width;
    g_height = height;
    g_window = xcb_generate_id(g_conn);
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE};
    xcb_create_window(g_conn, XCB_COPY_FROM_PARENT, g_window, screen->root, 0, 0, (uint16_t)width, (uint16_t)height,
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                      values);

    xcb_atom_t wmProtocols = internAtom("WM_PROTOCOLS");
    g_wmDelete = internAtom("WM_DELETE_WINDOW");
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_window, wmProtocols, XCB_ATOM_ATOM, 32, 1, &g_wmDelete);
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        (uint32_t)std::strlen(title), title);
    xcb_map_window(g_conn, g_window);
    xcb_flush(g_conn);
    return true;
}

void destroyWindow() {
    setCursorCaptured(false);
    if (g_joystick >= 0) close(g_joystick);
    g_joystick = -1;
    xcb_destroy_window(g_conn, g_window);
    xcb_disconnect(g_conn);
    g_conn = nullptr;
}

void pollEvents() {
    if (g_imguiReady) {
        ImGui_ImplXcb_PollEvents(onEvent, nullptr);
        pollJoystick();
        return;
    }
    while (xcb_generic_event_t* e = xcb_poll_for_event(g_conn)) {
        onEvent(nullptr, e);
        free(e);
    }
}

void setCursorCaptured(bool captured) {
    if (captured == g_captured || !g_conn) return;
    g_captured = captured;
    g_mouseDx = g_mouseDy = 0.f;
    if (captured) {
        if (!g_invisibleCursor) {   // a cursor made from an empty 1x1 bitmap
            xcb_pixmap_t pixmap = xcb_generate_id(g_conn);
            xcb_create_pixmap(g_conn, 1, pixmap, g_window, 1, 1);
            g_invisibleCursor = xcb_generate_id(g_conn);
            xcb_create_cursor(g_conn, g_invisibleCursor, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 0, 0);
            xcb_free_pixmap(g_conn, pixmap);
        }
        xcb_grab_pointer(g_conn, 1, g_window,
                         XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, g_window, g_invisibleCursor, XCB_CURRENT_TIME);
        warpToCenter();
    } else {
        xcb_ungrab_pointer(g_conn, XCB_CURRENT_TIME);
        xcb_flush(g_conn);
    }
}

bool cursorCaptured() { return g_captured; }

void takeMouseDelta(float& dx, float& dy) {
    dx = g_mouseDx;
    dy = g_mouseDy;
    g_mouseDx = g_mouseDy = 0.f;
}

// Asks the window manager (EWMH _NET_WM_STATE) to make the window fullscreen
void setFullscreen(bool fullscreen) {
    if (fullscreen == g_fullscreen || !g_conn) return;
    g_fullscreen = fullscreen;
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = g_window;
    ev.type = internAtom("_NET_WM_STATE");
    ev.data.data32[0] = fullscreen ? 1 : 0;   // _NET_WM_STATE_ADD / _REMOVE
    ev.data.data32[1] = internAtom("_NET_WM_STATE_FULLSCREEN");
    ev.data.data32[3] = 1;                    // request from a normal application
    xcb_send_event(g_conn, 0, g_root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   (const char*)&ev);
    xcb_flush(g_conn);
}

bool isFullscreen() { return g_fullscreen; }

bool setExecutableIcon(const std::filesystem::path&, const std::string&, std::string& error) {
    error = "executables have no icons here (the desktop entry has it)";
    return false;
}

// EWMH _NET_WM_ICON: width, height, then ARGB pixels as 32-bit values
void setWindowIcon(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!g_conn || !rgba || !width || !height) return;
    std::vector<uint32_t> data(2 + (size_t)width * height);
    data[0] = width;
    data[1] = height;
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        const uint8_t* p = rgba + i * 4;
        data[2 + i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE, g_window, internAtom("_NET_WM_ICON"), XCB_ATOM_CARDINAL, 32,
                        (uint32_t)data.size(), data.data());
    xcb_flush(g_conn);
}

bool closeRequested() { return g_closeRequested; }

void windowSize(uint32_t& width, uint32_t& height) {
    width = g_width;
    height = g_height;
}

float dpiScale() { return 1.f; }

const char* vulkanSurfaceExtension() { return VK_KHR_XCB_SURFACE_EXTENSION_NAME; }

VkSurfaceKHR createVulkanSurface(VkInstance instance) {
    VkXcbSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
    ci.connection = g_conn;
    ci.window = g_window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateXcbSurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
    return surface;
}

void imguiInit() {
    ImGui_ImplXcb_Init(g_conn, g_window);
    g_imguiReady = true;
    g_joystick = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
}

void imguiNewFrame(float deltaTime) { ImGui_ImplXcb_NewFrame((float)g_width, (float)g_height, deltaTime); }

void imguiShutdown() {
    ImGui_ImplXcb_Shutdown();
    g_imguiReady = false;
}

std::filesystem::path openFileDialog(const char* title, const char* extension) {
    // No toolkit here: borrow the desktop's dialog helper if one is installed
    std::string t = title, patterns;
    for (char& c : t) if (c == '\'' ) c = ' ';
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
    for (const std::string& cmd : cmds) {
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

std::filesystem::path openFolderDialog(const char* title) {
    std::string t = title;
    for (char& c : t) if (c == '\'') c = ' ';
    const std::string cmds[] = {"zenity --file-selection --directory --title='" + t + "' 2>/dev/null",
                                "kdialog --getexistingdirectory . --title '" + t + "' 2>/dev/null"};
    for (const std::string& cmd : cmds) {
        std::string tool = cmd.substr(0, cmd.find(' '));
        if (std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) != 0) continue;
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) continue;
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
    return {};
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
