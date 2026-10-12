// The standalone game: plays a scene without the editor.
//
//   vkgame [scene.vkw | project folder | --stress] [--fullscreen] [--language xx] [--aa msaa|taa|off]
//          [--host [port] | --join address[:port]] [--screenshot out.png [--frames N]] [--trace out.json]
//          [--lua-debug [port]]
//          [--check]   (loads everything but opens no window: for testing an exported game anywhere)
//
// Without a scene it plays the start scene of the project (a folder with project.json: the one given, the working
// directory or the executable's folder), then scene.vkw in those folders, and falls back to the built-in demo. The mouse is captured for the game: Esc releases it, a click takes it back.
// F11 toggles fullscreen.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "app/application.h"
#include "app/profiler_view.h"
#include "core/crash.h"
#include "script/lua_debugger.h"
#include "physics/fracture.h"
#include "physics/soft_body.h"
#include "core/log.h"
#include "core/profiler.h"
#include "audio/audio.h"
#include "core/project.h"
#include "core/vfs.h"
#include "assets/meshes.h"
#include "game/game.h"
#include "script/lua_script.h"
#include "assets/asset_db.h"
#include "net/net_session.h"
#include "scene/terrain.h"
#include "render/environment.h"
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
    std::string language;   // --language es: play in another language (strings/es.json) than the project's
    bool check = false;     // --check: load the game (pack, project, module, start scene) without a window, and exit
    std::string aa;         // --aa msaa | taa | off: anti-aliasing (the renderer's default: MSAA)
    std::string host, join; // --host [port] / --join address[:port]: a multiplayer game (net/net_session.h)
    std::string trace;      // --trace out.json: every frame's profile, saved as a Chrome trace on exit
    int luaDebugPort = 0;   // --lua-debug [port]: the Lua debugger listens there (script/lua_debugger.h)
    {
        int out = 1;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--language") && i + 1 < argc) language = argv[++i];
            else if (!std::strcmp(argv[i], "--aa") && i + 1 < argc) aa = argv[++i];
            else if (!std::strcmp(argv[i], "--host"))
                host = i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]) ? argv[++i] : "27015";
            else if (!std::strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
            else if (!std::strcmp(argv[i], "--check")) check = true;
            else if (!std::strcmp(argv[i], "--lua-debug"))
                luaDebugPort = i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]) ? std::atoi(argv[++i]) : 4711;
            else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
            else argv[out++] = argv[i];
        }
        argc = out;
    }
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
    if (haveProject) assetdb::open(project::root(), false);   // (asset ids: files moved since a scene was saved)
    if (haveProject) {   // its Lua behaviours (scripts/), also before the scene
        int scripts = scripting::loadProject(project::root());
        if (scripts) std::printf("Lua scripts: %d behaviours\n", scripts);
        for (const std::string& e : scripting::errors()) std::fprintf(stderr, "warning: script: %s\n", e.c_str());
    }
    error.clear();
    if (check) {   // everything a player's machine needs to start the game, short of a GPU
        if (path.empty()) {
            std::printf("check: no project or scene found (the built-in demo would play)\n");
            return stress ? 0 : 1;
        }
        Scene checked;
        if (!loadScene(path, checked, meshes, textures, error)) {
            logLine("check failed: %s", error.c_str());
            return 1;
        }
        int missing = 0, behaviours = 0;
        for (const Entity& e : checked.entities)
            for (const BehaviourPtr& b : e.behaviours) {
                ++behaviours;
                if (dynamic_cast<const MissingBehaviour*>(b.get())) {
                    ++missing;
                    std::printf("check: behaviour %s on %s has no code\n", b->typeName(), e.name.c_str());
                }
            }
        std::printf("check: %s: %zu entities, %d behaviours, %zu meshes, %d textures, %s%s\n", path.c_str(),
                    checked.entities.size(), behaviours, (size_t)meshes.count(), textures.count(),
                    vfs::mounted() ? "from a pack" : "from files", missing ? ": MISSING CODE" : ": ok");
        for (const std::string& e : scripting::errors()) std::printf("check: script error: %s\n", e.c_str());
        return missing || !scripting::errors().empty() ? 1 : 0;
    }
    if (stress) {
        scene = buildStressScene();
    } else if (path.empty()) {
        scene = buildDefaultScene();
    } else if (!vfs::exists(fs::u8path(path))) {   // (it loads once the window is up, behind the loading screen)
        logLine("fatal: cannot open %s", path.c_str());
        return 1;
    }
    // The log: in the user's data folder, next to the save games
    logfile::open(project::saveDir(settings.name.empty() ? "Untitled" : settings.name).parent_path() / "game.log");
    crash::install(project::saveDir(settings.name.empty() ? "Untitled" : settings.name).parent_path(), "game");
    if (!crash::previousReport().empty()) logLine("The last run crashed: %s", crash::previousReport().u8string().c_str());
    std::printf("Playing %s\n", stress ? "the stress test scene" : path.empty() ? "the built-in demo scene" : path.c_str());
    logfile::write("Playing " + (stress ? std::string("the stress test scene") : path.empty() ? std::string("the built-in demo scene") : path));

    Application app;
    if (!app.init(options, meshes, textures)) return 1;
    if (!trace.empty()) profiler::startRecording();
    if (luaDebugPort > 0) {
        std::string error;
        if (!scripting::debugger::listen((uint16_t)luaDebugPort, error)) logLine("lua debugger: %s", error.c_str());
    }
    if (aa == "taa") app.renderer().setAntialiasing(Renderer::Antialiasing::Temporal);
    else if (aa == "off") app.renderer().setAntialiasing(Renderer::Antialiasing::Off);
    else if (aa == "msaa") app.renderer().setAntialiasing(Renderer::Antialiasing::Msaa);
    if (!settings.icon.empty()) {   // the project's icon on the window (and the task bar / Dock)
        TextureLibrary icon;
        std::string iconError;
        int id = loadTexture(icon, project::resolve(settings.icon), iconError);
        if (id >= 0) platform::setWindowIcon(icon.get(id).rgba.data(), icon.get(id).width, icon.get(id).height);
        else logLine("icon: %s", iconError.c_str());
    }
    AudioEngine audio;
    if (!audio.init()) std::fprintf(stderr, "%s\n", audio.error().c_str());
    Game game;
    game.setAudio(&audio);
    game.setGamepadOutput({[](float low, float high, float seconds) { platform::gamepadRumble(low, high, seconds); },
                           [](Vec3 c) { platform::setGamepadLight(c.x, c.y, c.z); }});
    game.setAssetLibraries(meshes, textures);
    game.setLanguage(language.empty() ? settings.language : language);
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

    NetSession net;
    float dt = 0.f;
    bool firstLoad = !path.empty();
    bool showProfiler = false;   // F3
    while (!game.quitRequested() && app.beginFrame(dt)) {
        if (firstLoad && !game.loading()) {   // the start scene is in, or didn't load: that's fatal
            firstLoad = false;
            if (!game.loadError().empty()) {
                logLine("fatal: %s", game.loadError().c_str());
                app.shutdown();
                return 1;
            }
        }
        if (!firstLoad && !net.active() && (!host.empty() || !join.empty())) {   // (once the scene is in)
            std::string netError;
            bool ok = !host.empty() ? net.host(game, (uint16_t)std::atoi(host.c_str()), netError) : net.join(game, join, netError);
            if (!ok) logLine("multiplayer: %s", netError.c_str());
            host.clear();
            join.clear();
        }
        ImGuiIO& io = ImGui::GetIO();
        if (platform::cursorCaptured() && (uiWantsCursor || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            platform::setCursorCaptured(false);
        else if (!platform::cursorCaptured() && !uiWantsCursor && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            platform::setCursorCaptured(true);
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) platform::setFullscreen(!platform::isFullscreen());
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) showProfiler = !showProfiler;

        bool captured = platform::cursorCaptured();
        float mdx = 0.f, mdy = 0.f;
        platform::takeMouseDelta(mdx, mdy);
        game.input().setMouseCaptured(captured);
        scripting::poll();   // (scripts edited on disk while it runs: reloaded, keeping their state)
        {
            PROFILE_SCOPE("Game");
            game.frame(dt, !io.WantTextInput, true, captured ? ImVec2(mdx, mdy) : io.MouseDelta);
        }
        net.update(dt);   // (multiplayer: sends and receives after the game's updates)
        scripting::debugger::poll();

        uint32_t w = 0, h = 0;
        app.windowSize(w, h);
        ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, (float)w, (float)h, !captured);
        game.drawUI(ui);
        ui.end();
        uiWantsCursor = ui.wantsCursor();
        drawHud(game, captured || uiWantsCursor, app.uiScale());
        if (showProfiler) drawProfilerOverlay(app.uiScale());
        if (net.active()) {   // where the multiplayer game stands, in the top right corner
            const std::string& st = net.status();
            ImVec2 size = ImGui::CalcTextSize(st.c_str());
            ImGui::GetForegroundDrawList()->AddText(ImVec2((float)w - size.x - 12.f, 8.f), IM_COL32(255, 255, 255, 200),
                                                    st.c_str());
        }
        float aspect = h > 0 ? (float)w / (float)h : 1.f;
        CameraView cam;
        if (!game.activeCamera(cam)) cam.view = lookAt({0, 10, 40}, {0, 0, 0}, {0, 1, 0});   // no camera: a fixed view
        PROFILE_SCOPE("Draw and render");   // (to the end of the frame)
        std::vector<Mat4> worlds = game.worldMatrices();
        std::vector<Mat4> joints;
        terrain::syncMeshes(scene, meshes);
        softbody::syncMeshes(scene, meshes);
        fracture::syncMeshes(scene, meshes);
        std::vector<DrawItem> items = collectDrawItems(scene, worlds, kNoEntity, &joints, &meshes);
        std::vector<LightInfo> lights = collectLights(scene, worlds);
        std::vector<PlantSet> plants = collectPlants(scene, worlds, meshes);
        std::vector<ProbeInfo> probes = collectProbes(scene, worlds);
        std::vector<ParticleDraw> particles;
        game.particles().collect(particles);
        SceneRenderInfo info;
        info.items = &items;
        info.plants = &plants;
        info.lights = &lights;
        info.probes = &probes;
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
        info.colorFilter = game.colorFilter();
        if (!scene.render.skyImage.empty()) {   // (loaded and prefiltered once, then cached)
            std::string skyError;
            info.environment = environment::load(project::resolve(scene.render.skyImage), skyError);
        }
        info.skyIntensity = scene.render.skyIntensity;
        info.skyRotation = scene.render.skyRotation * DEG2RAD;
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
    if (!trace.empty()) {
        profiler::stopRecording();
        std::string error;
        if (profiler::saveChromeTrace(fs::u8path(trace), profiler::recorded(), error))
            std::printf("Saved a trace of %zu frames to %s\n", profiler::recorded().size(), trace.c_str());
        else
            logLine("trace: %s", error.c_str());
    }
    int code = app.exitCode();
    app.shutdown();
    return code;
}
