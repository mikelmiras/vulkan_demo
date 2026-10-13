// Linux window backend: X11 / XWayland via XCB (platform_linux.h).
#define VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "platform/platform.h"
#include "platform/platform_linux.h"

#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "platform/imgui_impl_xcb.h"

namespace platform::x11 {

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

bool g_focused = true;

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
            g_focused = false;
            setCursorCaptured(false);
            return false;
        case XCB_FOCUS_IN:
            g_focused = true;
            return false;
    }
    return false;
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
    xcb_destroy_window(g_conn, g_window);
    xcb_disconnect(g_conn);
    g_conn = nullptr;
}

void pollEvents() {
    if (g_imguiReady) {
        ImGui_ImplXcb_PollEvents(onEvent, nullptr);
        linux_shared::gamepadPoll();
        return;
    }
    while (xcb_generic_event_t* e = xcb_poll_for_event(g_conn)) {
        onEvent(nullptr, e);
        free(e);
    }
}

// (the joystick devices aren't watched: an idle wait is short enough that a pad is noticed anyway)
bool waitEvents(double seconds) {
    if (!g_conn) return false;
    xcb_flush(g_conn);
    pollfd fd{xcb_get_file_descriptor(g_conn), POLLIN, 0};
    return poll(&fd, 1, (int)std::clamp(seconds * 1000.0, 0.0, 60000.0)) > 0;
}

bool windowFocused() { return g_focused; }

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
    linux_shared::gamepadOpen();
}

void imguiNewFrame(float deltaTime) { ImGui_ImplXcb_NewFrame((float)g_width, (float)g_height, deltaTime); }

void imguiShutdown() {
    linux_shared::gamepadClose();
    ImGui_ImplXcb_Shutdown();
    g_imguiReady = false;
}

}  // namespace platform::x11
