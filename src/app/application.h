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
        std::string gpu;   // Renderer::setGpuPreference ("": the fastest)
    };

    // Parses the common options (--fullscreen, --screenshot, --frames) and removes them from argv;
    // what's left (the program name first) is for the caller
    static Options parseArgs(int& argc, char** argv, Options defaults);

    bool init(const Options& options, MeshLibrary& meshes, TextureLibrary& textures);
    void shutdown();
    // For automated runs (--screenshot): 1 if the screenshot failed or the validation layer reported errors
    int exitCode() const;

    // Frame loop: while (app.beginFrame(dt)) { ...build the UI and the scene... app.endFrame(info); }
    // beginFrame returns false when it's time to quit; it waits (and returns true with dt = 0 skipped) while
    // the window is minimised.
    bool beginFrame(float& dt);
    void endFrame(const SceneRenderInfo& scene);
    void requestQuit() { quit_ = true; }

    // Pacing: how often frames are drawn. By default every frame the display shows (a game). An editor sets `lazy`:
    // frames only while something happens (input, the window changing, requestRedraw: playing, a drag, a load, the
    // renderer still settling) and for `settleSeconds` after, else it sleeps until an event comes, drawing at least
    // every `idleSeconds` (stats, scripts changed on disk). maxFps caps the rate (0: the display's), unfocusedFps caps
    // it while another window is in front (0: no cap). Screenshot runs ignore it all.
    struct Pacing {
        bool lazy = false;
        float maxFps = 0.f;
        float unfocusedFps = 0.f;
        float settleSeconds = 0.75f;
        float idleSeconds = 2.f;
    };
    void setPacing(const Pacing& pacing) { pacing_ = pacing; }
    // What to do now (plain: tested): draw (after sleeping `sleep` seconds for the cap; `idle`: the heartbeat), or
    // wait up to `wait` seconds for an event and look again
    struct Decision {
        bool draw = true, idle = false;
        double sleep = 0.0, wait = 0.0;
    };
    static Decision decide(const Pacing& pacing, double now, double activeUntil, double lastFrame, bool focused);
    const Pacing& pacing() const { return pacing_; }
    void requestRedraw(float seconds = 0.f);   // keep drawing for this long (from now; 0: the next frame)
    bool drawingIdle() const { return idleFrame_; }   // this frame came from the idle heartbeat (nothing happening)
    double framesPerSecond() const { return fps_; }   // drawn, over the last second

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
    int screenshotResult_ = 0;   // 1 saved, -1 failed
    bool initialized_ = false;
    double lastTime_ = 0.0;
    Pacing pacing_;
    double activeUntil_ = 0.0, lastFrame_ = 0.0, fpsWindow_ = 0.0, fps_ = 0.0;
    int fpsFrames_ = 0;
    bool idleFrame_ = false;
    uint32_t lastWidth_ = 0, lastHeight_ = 0;
};
