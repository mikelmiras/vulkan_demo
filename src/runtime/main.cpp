// The standalone game: plays a scene without the editor.
//
//   vkgame [scene.vkw] [--fullscreen] [--screenshot out.png [--frames N]]
//
// Without a scene argument it looks for scene.vkw in the working directory, then next to the executable, and
// falls back to the built-in demo. The mouse is captured for the game: Esc releases it, a click takes it back.
// F11 toggles fullscreen.

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "app/application.h"
#include "audio/audio.h"
#include "assets/meshes.h"
#include "game/game.h"
#include "imgui.h"
#include "platform/platform.h"
#include "render/scene_draw.h"
#include "scene/serialization.h"

namespace {

std::string findScene(int argc, char** argv) {
    if (argc > 1) return argv[1];
    std::error_code ec;
    for (const std::filesystem::path& p : {std::filesystem::current_path(ec) / "scene.vkw", platform::executableDir() / "scene.vkw"})
        if (std::filesystem::exists(p, ec)) return p.u8string();
    return {};
}

// Messages logged by behaviours (fading out) and the mouse hint
void drawHud(const Game& game, bool captured, float scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float lineH = ImGui::GetTextLineHeightWithSpacing(), y = io.DisplaySize.y - 12.f * scale;
    int shown = 0;
    for (auto it = game.log().rbegin(); it != game.log().rend() && shown < 6; ++it, ++shown) {
        float age = game.time() - it->first;
        if (age > 6.f) break;
        int alpha = (int)(255.f * std::min(6.f - age, 1.f));
        y -= lineH;
        dl->AddText(ImVec2(13.f * scale, y + 1), IM_COL32(0, 0, 0, alpha / 2), it->second.c_str());
        dl->AddText(ImVec2(12.f * scale, y), IM_COL32(255, 235, 140, alpha), it->second.c_str());
    }
    char top[96];
    std::snprintf(top, sizeof(top), "%.0f FPS%s", io.Framerate, captured ? "" : "   |   click to play, Esc releases the mouse");
    dl->AddText(ImVec2(13.f * scale, 11.f * scale), IM_COL32(0, 0, 0, 140), top);
    dl->AddText(ImVec2(12.f * scale, 10.f * scale), IM_COL32(255, 255, 255, 200), top);
}

}  // namespace

int main(int argc, char** argv) {
    Application::Options defaults;
    defaults.title = "Vulkan World";
    Application::Options options = Application::parseArgs(argc, argv, defaults);

    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary textures = buildTextureLibrary();
    Scene scene;
    std::string path = findScene(argc, argv), error;
    if (path.empty()) {
        scene = buildDefaultScene();
    } else if (!loadScene(path, scene, meshes, textures, error)) {
        std::fprintf(stderr, "fatal: %s\n", error.c_str());
        return 1;
    }
    std::printf("Playing %s\n", path.empty() ? "the built-in demo scene" : path.c_str());

    Application app;
    if (!app.init(options, meshes, textures)) return 1;
    AudioEngine audio;
    if (!audio.init()) std::fprintf(stderr, "%s\n", audio.error().c_str());
    Game game;
    game.setAudio(&audio);
    game.start(scene, meshes);
    if (options.screenshotPath.empty()) platform::setCursorCaptured(true);

    float dt = 0.f;
    while (app.beginFrame(dt)) {
        ImGuiIO& io = ImGui::GetIO();
        if (platform::cursorCaptured() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) platform::setCursorCaptured(false);
        else if (!platform::cursorCaptured() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) platform::setCursorCaptured(true);
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) platform::setFullscreen(!platform::isFullscreen());

        bool captured = platform::cursorCaptured();
        float mdx = 0.f, mdy = 0.f;
        platform::takeMouseDelta(mdx, mdy);
        game.input().setMouseCaptured(captured);
        game.frame(dt, !io.WantTextInput, true, captured ? ImVec2(mdx, mdy) : io.MouseDelta);
        drawHud(game, captured, app.uiScale());

        uint32_t w = 0, h = 0;
        app.windowSize(w, h);
        float aspect = h > 0 ? (float)w / (float)h : 1.f;
        CameraView cam;
        if (!game.activeCamera(cam)) cam.view = lookAt({0, 10, 40}, {0, 0, 0}, {0, 1, 0});   // no camera: a fixed view
        std::vector<Mat4> worlds = game.worldMatrices();
        std::vector<DrawItem> items = collectDrawItems(scene, worlds);
        std::vector<LightInfo> lights = collectLights(scene, worlds);
        SceneRenderInfo info;
        info.items = &items;
        info.lights = &lights;
        info.camForward = normalize(transformDir(inverseAffine(cam.view), {0, 0, -1}));
        info.viewProj = perspective(cam.fovY, aspect, cam.zNear, cam.zFar, true) * cam.view;
        info.camPos = cam.position;
        info.viewportW = (float)w;
        info.viewportH = (float)h;
        app.endFrame(info);
    }
    game.stop();
    platform::setCursorCaptured(false);
    app.shutdown();
    return 0;
}
