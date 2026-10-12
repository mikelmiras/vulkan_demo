// Vulkan World Editor: a small Vulkan 3D scene with a Blender-style editing UI.
//
//   vkworld [project folder | scene.vkw] [--play] [--select <entity>] [--stress] [--fullscreen]
//           [--screenshot out.png [--frames N]]
//   vkworld <project folder> --export <folder> [--pack | --single-file]   (no window: exports the game and quits)
//
// The project is the folder given, else the working directory (its start scene opens if it has a project.json).
// --play starts play mode right away; --select selects an entity by name; --stress opens the stress test scene;
// --screenshot saves frame N (default 30) and quits.

#include <cstdio>
#include <cstring>

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
    Application::Options options = Application::parseArgs(argc, argv, defaults);
    const char* scenePath = nullptr;
    const char* selectName = nullptr;
    const char* exportDir = nullptr;
    ExportOptions exportOptions;
    bool play = false, stress = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--play")) play = true;
        else if (!std::strcmp(argv[i], "--stress")) stress = true;
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) selectName = argv[++i];
        else if (!std::strcmp(argv[i], "--export") && i + 1 < argc) exportDir = argv[++i];
        else if (!std::strcmp(argv[i], "--pack")) exportOptions.layout = ExportOptions::Layout::Pack;
        else if (!std::strcmp(argv[i], "--single-file")) exportOptions.layout = ExportOptions::Layout::SingleFile;
        else scenePath = argv[i];
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
        return 0;
    }

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
    // (screenshot runs always start from the default layout and preferences, and leave the saved ones alone)
    Editor editor(meshes, textures, &audio, app.uiScale(), options.screenshotPath.empty());
    editor.setUiTextures(app.uiTextures(textures));
    editor.setRenderer(&app.renderer());
    if (projectArg) editor.openProject(scenePath);
    else if (scenePath) editor.openScene(scenePath);
    else if (project::exists(project::root())) editor.openProject(project::root());
    if (stress) editor.newStressScene();
    if (selectName && !editor.selectByName(selectName)) std::fprintf(stderr, "no entity named %s\n", selectName);
    if (play) editor.play();

    float dt = 0.f;
    while (!editor.quitRequested() && app.beginFrame(dt)) {
        editor.frame(dt, app.renderer().gpuName(), app.renderer().stats());
        app.endFrame(editor.renderInfo());
    }
    int code = app.exitCode();
    app.shutdown();
    return code;
}
