// Linux platform layer: X11 / XWayland window via XCB.
#define VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "imgui_impl_xcb.h"

namespace platform {

namespace {

xcb_connection_t* g_conn = nullptr;
xcb_window_t g_window = 0;
xcb_atom_t g_wmDelete = 0;
uint32_t g_width = 0, g_height = 0;
bool g_closeRequested = false;
bool g_imguiReady = false;

xcb_atom_t internAtom(const char* name) {
    auto cookie = xcb_intern_atom(g_conn, 0, (uint16_t)std::strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(g_conn, cookie, nullptr);
    xcb_atom_t atom = reply ? reply->atom : 0;
    free(reply);
    return atom;
}

// Window-level events the ImGui backend doesn't consume
void onEvent(void*, xcb_generic_event_t* e) {
    switch (e->response_type & 0x7f) {
        case XCB_CONFIGURE_NOTIFY: {
            auto* c = (xcb_configure_notify_event_t*)e;
            g_width = c->width;
            g_height = c->height;
            break;
        }
        case XCB_CLIENT_MESSAGE:
            if (((xcb_client_message_event_t*)e)->data.data32[0] == g_wmDelete) g_closeRequested = true;
            break;
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
    xcb_destroy_window(g_conn, g_window);
    xcb_disconnect(g_conn);
    g_conn = nullptr;
}

void pollEvents() {
    if (g_imguiReady) {
        ImGui_ImplXcb_PollEvents(onEvent, nullptr);
        return;
    }
    while (xcb_generic_event_t* e = xcb_poll_for_event(g_conn)) {
        onEvent(nullptr, e);
        free(e);
    }
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
}

void imguiNewFrame(float deltaTime) { ImGui_ImplXcb_NewFrame((float)g_width, (float)g_height, deltaTime); }

void imguiShutdown() {
    ImGui_ImplXcb_Shutdown();
    g_imguiReady = false;
}

std::filesystem::path openFileDialog(const char* title, const char* extension) {
    // No toolkit here: borrow the desktop's dialog helper if one is installed
    std::string t = title, e = extension;
    for (char& c : t) if (c == '\'' ) c = ' ';
    const std::string cmds[] = {
        "zenity --file-selection --title='" + t + "' --file-filter='*." + e + "' --file-filter='*' 2>/dev/null",
        "kdialog --getopenfilename . '*." + e + "' --title '" + t + "' 2>/dev/null"};
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

std::filesystem::path executableDir() {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : exe.parent_path();
}

std::vector<std::filesystem::path> uiFontCandidates() {
    return {"/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"};
}

}  // namespace platform
