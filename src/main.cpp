// Vulkan World Editor: a small Vulkan 3D scene with a Blender-style editing UI.
//
//   vkworld [scene.vkw] [--play] [--select <entity>] [--fullscreen] [--screenshot out.png [--frames N]]
//
// --play starts play mode right away; --select selects an entity by name; --screenshot saves frame N (default
// 30) and quits.

#include <cstdio>
#include <cstring>

#include "app/application.h"
#include "assets/meshes.h"
#include "editor/editor.h"

int main(int argc, char** argv) {
    Application::Options defaults;
    defaults.title = "Vulkan World Editor";
    Application::Options options = Application::parseArgs(argc, argv, defaults);
    const char* scenePath = nullptr;
    const char* selectName = nullptr;
    bool play = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--play")) play = true;
        else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) selectName = argv[++i];
        else scenePath = argv[i];
    }

    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary textures = buildTextureLibrary();
    Application app;
    if (!app.init(options, meshes, textures)) return 1;

    Editor editor(meshes, textures, app.uiScale());
    if (scenePath) editor.openScene(scenePath);
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
