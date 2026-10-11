// What every program built on the engine needs: the window, the renderer, Dear ImGui and the frame loop.
// Shared by the editor (vkworld) and the standalone game (vkgame).
#pragma once

#include <string>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "game/ui.h"
#include "render/renderer.h"

class Application {
public:
    struct Options {
        std::string title = "Vulkan World";
        uint32_t width = 1600, height = 900;
        bool fullscreen = false;
        // --screenshot <file> [--frames N]: save frame N as a PNG and quit (for automated checks)
        std::string screenshotPath;
        int screenshotFrame = 30;
    };

    // Parses the common options (--fullscreen, --screenshot, --frames) and removes them from argv;
    // what's left (the program name first) is for the caller
    static Options parseArgs(int& argc, char** argv, Options defaults);

    bool init(const Options& options, MeshLibrary& meshes, TextureLibrary& textures);
    void shutdown();

    // Frame loop: while (app.beginFrame(dt)) { ...build the UI and the scene... app.endFrame(info); }
    // beginFrame returns false when it's time to quit; it waits (and returns true with dt = 0 skipped) while
    // the window is minimised.
    bool beginFrame(float& dt);
    void endFrame(const SceneRenderInfo& scene);
    void requestQuit() { quit_ = true; }

    Renderer& renderer() { return renderer_; }
    // Texture names for GameUI::image(): library textures, or image files (relative to the project) loaded into
    // `textures` on first use
    GameUI::TextureLookup uiTextures(TextureLibrary& textures);
    float uiScale() const { return uiScale_; }
    void windowSize(uint32_t& width, uint32_t& height) const { width = width_; height = height_; }

private:
    Options options_;
    Renderer renderer_;
    float uiScale_ = 1.f;
    uint32_t width_ = 0, height_ = 0;
    int frameCount_ = 0;
    bool quit_ = false;
    bool initialized_ = false;
    double lastTime_ = 0.0;
};
