// Vulkan World Editor: a small Vulkan 3D scene with a Blender-style editing UI.
//
//   vkworld [project folder | scene.vkw] [--play [--game-view]] [--select <entity>] [--stress] [--fullscreen]
//           [--screenshot out.png [--frames N]]
//   vkworld <project folder> --export <folder> [--pack | --single-file] [--package]   (no window: exports the game
//           and quits; --package: an AppImage, .dmg or installer of it too)
//
// The project is the folder given, else the working directory (its start scene opens if it has a project.json).
// --play starts play mode right away (--game-view: with the Game view window shown); --select selects an entity by
// name; --stress opens the stress test scene;
// --screenshot saves frame N (default 30) and quits.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/crash.h"
#include "core/log.h"
#include <filesystem>

#include "app/application.h"
#include "app/export.h"
#include "platform/platform.h"
#include "assets/meshes.h"
#include "audio/audio.h"
#include "core/project.h"
#include "editor/editor.h"

int main(int argc, char** argv) {
    Application::Options defaults;
    defaults.title = "Vulkan World Editor";
#if defined(__APPLE__)
    defaults.gpu = "integrated";   // (a MacBook with two GPUs: the editor on the one that saves power; VKWORLD_GPU)
#endif
    Application::Options options = Application::parseArgs(argc, argv, defaults);
    const char* scenePath = nullptr;
    const char* selectName = nullptr;
    const char* exportDir = nullptr;
    const char* buildName = nullptr;   // --build <profile>: the project's build profile, then out
    ExportOptions exportOptions;
    bool play = false, stress = false, gameView = false, launcher = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--play")) play = true;
        else if (!std::strcmp(argv[i], "--launcher")) launcher = true;   // (the project launcher even with a project)
        else if (!std::strcmp(argv[i], "--game-view")) gameView = true;
        else if (!std::strcmp(argv[i], "--stress")) stress = true;
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) selectName = argv[++i];
        else if (!std::strcmp(argv[i], "--export") && i + 1 < argc) exportDir = argv[++i];
        else if (!std::strcmp(argv[i], "--build") && i + 1 < argc) buildName = argv[++i];
        else if (!std::strcmp(argv[i], "--pack")) exportOptions.layout = ExportOptions::Layout::Pack;
        else if (!std::strcmp(argv[i], "--single-file")) exportOptions.layout = ExportOptions::Layout::SingleFile;
        else if (!std::strcmp(argv[i], "--package")) exportOptions.package = true;
        else if (!std::strcmp(argv[i], "--update-pack")) exportOptions.updatePack = true;
        else if (!std::strcmp(argv[i], "--cook")) exportOptions.cook = true;   // (textures and models cooked)
        else if (!std::strcmp(argv[i], "--textures") && i + 1 < argc) {   // ... compressed as bc7, bc1 or none
            exportOptions.cook = true;
            exportOptions.textureCompression = argv[++i];
        }
        else scenePath = argv[i];
    }

    if (buildName) {   // a build profile of the project (project.json "builds"), as the editor's Build Settings do
        if (!scenePath) { std::fprintf(stderr, "--build: give the project folder too\n"); return 1; }
        project::Settings settings;
        std::string error;
        if (!project::load(std::filesystem::u8path(scenePath), settings, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        auto it = std::find_if(settings.builds.begin(), settings.builds.end(), [&](const auto& b) { return b.name == buildName; });
        if (it == settings.builds.end()) { std::fprintf(stderr, "--build: the project has no build profile named %s\n", buildName); return 1; }
        ExportResult result;
        std::filesystem::path out = buildFolder(std::filesystem::u8path(scenePath), *it);
        if (!exportGame(scenePath, out, platform::executableDir(), result, error, optionsFor(*it))) {
            std::fprintf(stderr, "build failed: %s\n", error.c_str());
            return 1;
        }
        for (const std::string& w : result.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        std::printf("Built %s (%s): %s\n", it->name.c_str(), it->shipping ? "shipping" : "development", result.executable.u8string().c_str());
        return 0;
    }
    if (exportDir) {
        if (!scenePath) { std::fprintf(stderr, "--export: give the project folder too\n"); return 1; }
        ExportResult result;
        std::string error;
        if (!exportGame(scenePath, exportDir, platform::executableDir(), result, error, exportOptions)) {
            std::fprintf(stderr, "export failed: %s\n", error.c_str());
            return 1;
        }
        for (const std::string& w : result.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        std::printf("Exported %d files%s; the game: %s\n", result.files, result.standalone ? " (standalone)" : "",
                    result.executable.u8string().c_str());
        if (result.cooked) std::printf("Cooked %d textures and models\n", result.cooked);
        if (!result.package.empty()) std::printf("Packaged: %s\n", result.package.u8string().c_str());
        if (!result.updatePack.empty())
            std::printf("Update pack: %s (sha256 %s)\n", result.updatePack.u8string().c_str(), result.updateSha256.c_str());
        return 0;
    }

    options.pipelineCache = project::saveDir("Editor").parent_path() / "pipelines-editor.cache";   // (later starts: faster)
    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary textures = buildTextureLibrary();
    Application app;
    if (!app.init(options, meshes, textures)) return 1;

    AudioEngine audio;
    if (!audio.init()) std::fprintf(stderr, "%s\n", audio.error().c_str());
    std::error_code ec;
    bool projectArg = scenePath && std::filesystem::is_directory(scenePath, ec);
    if (projectArg) project::setRoot(scenePath);
    logfile::open(project::saveDir("Editor").parent_path() / "editor.log");
    crash::install(project::saveDir("Editor").parent_path(), "editor");
    if (!crash::previousReport().empty()) logLine("The last session crashed: %s", crash::previousReport().u8string().c_str());
    // (screenshot runs always start from the default layout and preferences, and leave the saved ones alone)
    Editor editor(meshes, textures, &audio, app.uiScale(), options.screenshotPath.empty());
    editor.setUiTextures(app.uiTextures(textures));
    editor.setRenderer(&app.renderer());
    if (const char* hdr = std::getenv("VKWORLD_HDR"); hdr && !std::strcmp(hdr, "1") && options.screenshotPath.empty())
        app.renderer().setHdrOutput(true);   // (where the display takes it; View > HDR output)
    if (projectArg) editor.openProject(scenePath);
    else if (scenePath) editor.openScene(scenePath);
    else if (project::exists(project::root())) editor.openProject(project::root());
    if (stress) editor.newStressScene();
    if (selectName && !editor.selectByName(selectName)) std::fprintf(stderr, "no entity named %s\n", selectName);
    if (gameView) editor.setGameView(true);
    // Nothing to open (no project or scene given, none found): the project launcher, as other engines start
    if (launcher || (!projectArg && !scenePath && !stress && !play && !project::exists(project::root()) && options.screenshotPath.empty()))
        editor.showLauncher();
    if (play) editor.play();
    if (!crash::previousReport().empty())
        editor.showStatus("The last session crashed. Its report: " + crash::previousReport().u8string());

    float dt = 0.f;
    while (!editor.quitRequested() && app.beginFrame(dt)) {
        editor.setIdle(app.drawingIdle());
        if (editor.uiScale() != app.uiScale()) editor.setUiScale(app.uiScale());   // (moved to another display)
        editor.frame(dt, app.renderer().gpuName(), app.renderer().stats());
        Application::Pacing pacing = app.pacing();   // (the editor sleeps while nothing changes: View > Power)
        editor.pacing(pacing.lazy, pacing.maxFps, pacing.unfocusedFps);
        app.setPacing(pacing);
        if (editor.busy()) app.requestRedraw();
        app.endFrame(editor.renderInfo());
    }
    int code = app.exitCode();
    app.shutdown();
    return code;
}
