// The standalone game: plays a scene without the editor.
//
//   vkgame [scene.vkw | project folder | --stress] [--fullscreen] [--screenshot out.png [--frames N]]
//
// Without a scene it plays the start scene of the project (a folder with project.json: the one given, the working
// directory or the executable's folder), then scene.vkw in those folders, and falls back to the built-in demo. The mouse is captured for the game: Esc releases it, a click takes it back.
// F11 toggles fullscreen.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "app/application.h"
#include "audio/audio.h"
#include "core/project.h"
#include "core/vfs.h"
#include "assets/meshes.h"
#include "game/game.h"
#include "game/module.h"
#include "imgui.h"
#include "platform/platform.h"
#include "render/scene_draw.h"
#include "scene/serialization.h"
#include "scene/stress_scene.h"

#ifdef VKWORLD_STANDALONE_MODULE
extern "C" void vkworldModuleRegister();   // VKWORLD_GAME_MODULE() in the game's sources
#endif

namespace {

namespace fs = std::filesystem;

// Picks the project (setting project::root) and returns the scene to play, or "" for the demo
std::string findScene(int argc, char** argv) {
    std::error_code ec;
    if (argc > 1 && !fs::is_directory(argv[1], ec)) {   // a scene file: its project is the folder above it with one
        fs::path file = fs::absolute(fs::u8path(argv[1]), ec);
        for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            if (project::exists(dir)) { project::setRoot(dir); break; }
            if (dir == dir.parent_path()) break;
        }
        return file.u8string();
    }
    std::vector<fs::path> folders;
    if (argc > 1) folders.push_back(argv[1]);
    else folders = {fs::current_path(ec), platform::executableDir()};
    for (const fs::path& dir : folders) {
        project::Settings settings;
        std::string error;
        if (project::exists(dir) && project::load(dir, settings, error)) {
            project::setRoot(dir);
            return project::resolve(settings.startScene);
        }
        if (fs::exists(dir / "scene.vkw", ec)) {
            project::setRoot(dir);
            return (dir / "scene.vkw").u8string();
        }
    }
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
    char top[128];
    std::snprintf(top, sizeof(top), "%.0f FPS  |  update %.2f ms%s", io.Framerate, game.updateMs(),
                  captured ? "" : "   |   click to play, Esc releases the mouse");
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
    // A packed game: its files in a pack appended to this executable, or in game.pak next to it
    if (vfs::mount(platform::executablePath(), platform::executableDir()) ||
        vfs::mount(platform::executableDir() / "game.pak", platform::executableDir()))
        std::printf("Playing from a pack\n");
    GameModule module;   // declared before the scene: outlives the behaviours it made
    Scene scene;
    bool stress = argc > 1 && !std::strcmp(argv[1], "--stress");
    std::string path = stress ? "" : findScene(argc, argv), error;
    // The project's own behaviours: before the scene loads, so it finds them
    project::Settings settings;
    bool haveProject = project::exists(project::root()) && project::load(project::root(), settings, error);
#ifdef VKWORLD_STANDALONE_MODULE
    vkworldModuleRegister();   // the module's code is compiled into this executable
    (void)haveProject;
#else
    if (haveProject && !settings.module.empty()) {
        fs::path lib = GameModule::libraryPath(fs::u8path(project::resolve(settings.module)));
        if (module.load(lib, error)) std::printf("Game module %s: %zu behaviours\n", lib.u8string().c_str(), module.behaviourTypes().size());
        else std::fprintf(stderr, "warning: %s\n", error.c_str());
    }
#endif
    error.clear();
    if (stress) {
        scene = buildStressScene();
    } else if (path.empty()) {
        scene = buildDefaultScene();
    } else if (!vfs::exists(fs::u8path(path))) {   // (it loads once the window is up, behind the loading screen)
        std::fprintf(stderr, "fatal: cannot open %s\n", path.c_str());
        return 1;
    }
    std::printf("Playing %s\n", stress ? "the stress test scene" : path.empty() ? "the built-in demo scene" : path.c_str());

    Application app;
    if (!app.init(options, meshes, textures)) return 1;
    AudioEngine audio;
    if (!audio.init()) std::fprintf(stderr, "%s\n", audio.error().c_str());
    Game game;
    game.setAudio(&audio);
    game.setAssetLibraries(meshes, textures);
    game.setAsyncLoading(true);   // scene changes load in the background, behind a loading screen
    game.setScenePath(path.empty() ? "" : project::toStored(path));
    game.setSaveDirectory(project::saveDir(settings.name.empty() ? "Untitled" : settings.name));
    game.start(scene, meshes);   // (empty until the scene file has loaded)
    if (!path.empty()) {
        game.loadScene(project::toStored(path));
        if (!options.screenshotPath.empty()) game.finishLoading();   // screenshots show the scene, not the loading
    }
    if (options.screenshotPath.empty()) platform::setCursorCaptured(true);

    GameUI ui;
    ui.setTextureLookup(app.uiTextures(textures));
    bool uiWantsCursor = false;   // the game's UI had a menu up last frame: keep the pointer free

    float dt = 0.f;
    bool firstLoad = !path.empty();
    while (!game.quitRequested() && app.beginFrame(dt)) {
        if (firstLoad && !game.loading()) {   // the start scene is in, or didn't load: that's fatal
            firstLoad = false;
            if (!game.loadError().empty()) {
                std::fprintf(stderr, "fatal: %s\n", game.loadError().c_str());
                app.shutdown();
                return 1;
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        if (platform::cursorCaptured() && (uiWantsCursor || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            platform::setCursorCaptured(false);
        else if (!platform::cursorCaptured() && !uiWantsCursor && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            platform::setCursorCaptured(true);
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) platform::setFullscreen(!platform::isFullscreen());

        bool captured = platform::cursorCaptured();
        float mdx = 0.f, mdy = 0.f;
        platform::takeMouseDelta(mdx, mdy);
        game.input().setMouseCaptured(captured);
        game.frame(dt, !io.WantTextInput, true, captured ? ImVec2(mdx, mdy) : io.MouseDelta);

        uint32_t w = 0, h = 0;
        app.windowSize(w, h);
        ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, (float)w, (float)h, !captured);
        game.drawUI(ui);
        ui.end();
        uiWantsCursor = ui.wantsCursor();
        drawHud(game, captured || uiWantsCursor, app.uiScale());
        float aspect = h > 0 ? (float)w / (float)h : 1.f;
        CameraView cam;
        if (!game.activeCamera(cam)) cam.view = lookAt({0, 10, 40}, {0, 0, 0}, {0, 1, 0});   // no camera: a fixed view
        std::vector<Mat4> worlds = game.worldMatrices();
        std::vector<Mat4> joints;
        std::vector<DrawItem> items = collectDrawItems(scene, worlds, kNoEntity, &joints, &meshes);
        std::vector<LightInfo> lights = collectLights(scene, worlds);
        std::vector<ParticleDraw> particles;
        game.particles().collect(particles);
        SceneRenderInfo info;
        info.items = &items;
        info.lights = &lights;
        info.joints = &joints;
        info.particles = &particles;
        info.sky = scene.render.sky;
        info.zenith = scene.render.zenith;
        info.ground = scene.render.ground;
        info.fovY = cam.fovY;
        info.aspect = aspect;
        info.fogDensity = scene.render.fogDensity;
        info.ambient = scene.render.ambient;
        info.exposure = scene.render.exposure;
        info.bloomStrength = scene.render.bloomStrength;
        info.bloomThreshold = scene.render.bloomThreshold;
        info.aoStrength = scene.render.aoStrength;
        info.aoRadius = scene.render.aoRadius;
        info.zNear = cam.zNear;
        info.zFar = cam.zFar;
        info.camForward = normalize(transformDir(inverseAffine(cam.view), {0, 0, -1}));
        info.viewProj = perspective(cam.fovY, aspect, cam.zNear, cam.zFar, true) * cam.view;
        info.camPos = cam.position;
        info.viewportW = (float)w;
        info.viewportH = (float)h;
        app.endFrame(info);
    }
    game.stop();
    platform::setCursorCaptured(false);
    int code = app.exitCode();
    app.shutdown();
    return code;
}
