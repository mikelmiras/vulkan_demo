// Vulkan World Editor: a small Vulkan 3D scene with a Blender-style editing UI.
// Window/input: platform layer (XCB on Linux, Win32 on Windows). Rendering: Vulkan 1.3. UI: Dear ImGui + ImGuizmo.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>

#include "editor.h"
#include "geometry.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "platform.h"
#include "renderer.h"

namespace {

void setupImGuiStyle(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // fixed layout, nothing worth persisting
    for (const auto& font : platform::uiFontCandidates()) {
        std::error_code ec;
        if (std::filesystem::exists(font, ec) && io.Fonts->AddFontFromFileTTF(font.u8string().c_str(), 16.f * scale))
            break;
    }

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
    if (scale > 1.f) s.ScaleAllSizes(scale);
}

}  // namespace

int main() {
    if (!platform::createWindow("Vulkan World Editor", 1600, 900)) {
        std::fprintf(stderr, "fatal: cannot create window\n");
        return 1;
    }
    uint32_t width = 0, height = 0;
    platform::windowSize(width, height);

    MeshLibrary meshes = buildMeshLibrary();
    Renderer renderer;
    renderer.init(width, height, meshes);

    const float uiScale = std::max(platform::dpiScale(), 1.f);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    setupImGuiStyle(uiScale);
    platform::imguiInit();
    renderer.initImGui();

    Editor editor(meshes, uiScale);

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    while (!platform::closeRequested() && !editor.quitRequested()) {
        platform::pollEvents();
        platform::windowSize(width, height);
        renderer.resize(width, height);

        auto now = clock::now();
        float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;

        if (width == 0 || height == 0) {   // minimised
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        platform::imguiNewFrame(dt);
        ImGui::NewFrame();
        editor.frame(dt, renderer.gpuName(), renderer.stats());
        ImGui::Render();
        renderer.render(editor.renderInfo(), ImGui::GetDrawData());
    }

    renderer.shutdown();   // also shuts down the ImGui Vulkan backend
    platform::imguiShutdown();
    ImGui::DestroyContext();
    platform::destroyWindow();
    return 0;
}
