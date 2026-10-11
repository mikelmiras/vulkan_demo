// Vulkan World Editor: a small Vulkan 3D scene with a Blender-style editing UI.
//
//   vkworld [project folder | scene.vkw] [--play] [--select <entity>] [--stress] [--fullscreen]
//           [--screenshot out.png [--frames N]]
//
// The project is the folder given, else the working directory (its start scene opens if it has a project.json).
// --play starts play mode right away; --select selects an entity by name; --stress opens the stress test scene;
// --screenshot saves frame N (default 30) and quits.

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "app/application.h"
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
    bool play = false, stress = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--play")) play = true;
        else if (!std::strcmp(argv[i], "--stress")) stress = true;
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) selectName = argv[++i];
        else scenePath = argv[i];
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
    Editor editor(meshes, textures, &audio, app.uiScale());
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
    app.shutdown();
    return 0;
}
