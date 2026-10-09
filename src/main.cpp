// Vulkan World Editor: a small Vulkan 3D scene with a Blender-style editing UI.
// Window/input: raw XCB (Linux/X11 or XWayland). Rendering: Vulkan 1.3. UI: Dear ImGui + ImGuizmo.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "editor.h"
#include "geometry.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_xcb.h"
#include "renderer.h"

namespace {

struct Window {
    xcb_connection_t* conn = nullptr;
    xcb_window_t id = 0;
    xcb_atom_t wmProtocols = 0, wmDelete = 0;
    uint32_t width = 1600, height = 900;
    bool closeRequested = false;

    xcb_atom_t internAtom(const char* name) {
        auto cookie = xcb_intern_atom(conn, 0, (uint16_t)std::strlen(name), name);
        xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(conn, cookie, nullptr);
        xcb_atom_t atom = reply ? reply->atom : 0;
        free(reply);
        return atom;
    }

    void create(const char* title) {
        int screenNum = 0;
        conn = xcb_connect(nullptr, &screenNum);
        if (xcb_connection_has_error(conn)) {
            std::fprintf(stderr, "fatal: cannot connect to X server (is DISPLAY set?)\n");
            std::exit(1);
        }
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
        for (int i = 0; i < screenNum; ++i) xcb_screen_next(&it);
        xcb_screen_t* screen = it.data;

        id = xcb_generate_id(conn);
        uint32_t values[2] = {
            screen->black_pixel,
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE | XCB_EVENT_MASK_BUTTON_PRESS |
                XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW |
                XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, id, screen->root, 0, 0, (uint16_t)width, (uint16_t)height, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                          values);

        wmProtocols = internAtom("WM_PROTOCOLS");
        wmDelete = internAtom("WM_DELETE_WINDOW");
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, id, wmProtocols, XCB_ATOM_ATOM, 32, 1, &wmDelete);
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            (uint32_t)std::strlen(title), title);
        xcb_map_window(conn, id);
        xcb_flush(conn);
    }

    // Window-level events the ImGui backend doesn't consume
    static void onEvent(void* user, xcb_generic_event_t* e) {
        auto* w = (Window*)user;
        switch (e->response_type & 0x7f) {
            case XCB_CONFIGURE_NOTIFY: {
                auto* c = (xcb_configure_notify_event_t*)e;
                w->width = c->width;
                w->height = c->height;
                break;
            }
            case XCB_CLIENT_MESSAGE:
                if (((xcb_client_message_event_t*)e)->data.data32[0] == w->wmDelete) w->closeRequested = true;
                break;
        }
    }

    void destroy() {
        xcb_destroy_window(conn, id);
        xcb_disconnect(conn);
    }
};

void setupImGuiStyle() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // fixed layout, nothing worth persisting
    const char* fonts[] = {"/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* f : fonts)
        if (access(f, R_OK) == 0 && io.Fonts->AddFontFromFileTTF(f, 16.f)) break;

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.f;
    s.FrameRounding = 3.f;
    s.GrabRounding = 3.f;
    s.PopupRounding = 4.f;
    s.WindowBorderSize = 1.f;
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(7, 5);
    s.Colors[ImGuiCol_WindowBg].w = 0.96f;
}

}  // namespace

int main() {
    Window window;
    window.create("Vulkan World Editor");

    MeshLibrary meshes = buildMeshLibrary();
    Renderer renderer;
    renderer.init(window.conn, window.id, window.width, window.height, meshes);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    setupImGuiStyle();
    ImGui_ImplXcb_Init(window.conn, window.id);
    renderer.initImGui();

    Editor editor(meshes);

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    while (!window.closeRequested && !editor.quitRequested()) {
        ImGui_ImplXcb_PollEvents(&Window::onEvent, &window);
        renderer.resize(window.width, window.height);

        auto now = clock::now();
        float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        if (window.width == 0 || window.height == 0) { usleep(16000); continue; }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplXcb_NewFrame((float)window.width, (float)window.height, dt);
        ImGui::NewFrame();
        editor.frame(dt, renderer.gpuName(), renderer.stats());
        ImGui::Render();
        renderer.render(editor.renderInfo(), ImGui::GetDrawData());
    }

    renderer.shutdown();   // also shuts down the ImGui Vulkan backend
    ImGui_ImplXcb_Shutdown();
    ImGui::DestroyContext();
    window.destroy();
    return 0;
}
