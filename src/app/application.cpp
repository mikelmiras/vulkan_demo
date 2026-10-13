#include "app/application.h"
#include "core/profiler.h"
#include "game/fonts.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <system_error>
#include <thread>

#include "core/project.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "core/version.h"
#include "platform/platform.h"

namespace {

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The look, at a scale (sizes and fonts: the window's in physical pixels on HiDPI displays). Again whenever the
// display's scale changes (a window moved to another monitor): fonts are drawn at the size asked for (ImGui 1.92)
void applyStyle(float scale) {
    ImGuiStyle fresh;
    ImGui::GetStyle() = fresh;
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
    s.FontScaleDpi = scale;
}

void setupImGuiStyle(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // (the editor keeps its panel layout in a file of its own: editor_prefs.h)
    for (const auto& font : platform::uiFontCandidates()) {
        std::error_code ec;
        if (std::filesystem::exists(font, ec) && io.Fonts->AddFontFromFileTTF(font.u8string().c_str(), 16.f)) break;
    }
    applyStyle(scale);
}

}  // namespace

Application::Options Application::parseArgs(int& argc, char** argv, Options o) {
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--version")) {   // (and nothing else)
            std::printf("%s %s\n", std::filesystem::path(argv[0]).stem().u8string().c_str(), version::full().c_str());
            std::exit(0);
        } else if (!std::strcmp(argv[i], "--fullscreen")) {
            o.fullscreen = true;
        } else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            o.screenshotPath = argv[++i];
        } else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            o.screenshotFrame = std::max(1, std::atoi(argv[++i]));
        } else {
            argv[out++] = argv[i];
        }
    }
    argc = out;
    return o;
}

bool Application::init(const Options& options, MeshLibrary& meshes, TextureLibrary& textures) {
    options_ = options;
    if (!platform::createWindow(options.title.c_str(), options.width, options.height)) {
        std::fprintf(stderr, "fatal: cannot create window\n");
        return false;
    }
    if (options.fullscreen) platform::setFullscreen(true);
    platform::windowSize(width_, height_);
    renderer_.setGpuPreference(options.gpu);
    renderer_.init(width_, height_, meshes, textures);

    uiScale_ = std::max(platform::dpiScale(), 1.f);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    setupImGuiStyle(uiScale_);
    platform::imguiInit();
    renderer_.initImGui();
    lastTime_ = nowSeconds();
    initialized_ = true;
    return true;
}

void Application::shutdown() {
    if (!initialized_) return;
    renderer_.shutdown();   // also shuts down the ImGui Vulkan backend
    platform::imguiShutdown();
    ImGui::DestroyContext();
    platform::destroyWindow();
    initialized_ = false;
}

Application::Decision Application::decide(const Pacing& pacing, double now, double activeUntil, double lastFrame, bool focused) {
    Decision d;
    if (pacing.lazy && now >= activeUntil) {   // nothing happening
        double wait = pacing.idleSeconds - (now - lastFrame);
        if (wait > 0.0) {
            d.draw = false;
            d.wait = wait;
            return d;
        }
        d.idle = true;
    }
    float cap = pacing.maxFps;   // (lower while in the background)
    if (pacing.unfocusedFps > 0.f && !focused) cap = cap > 0.f ? std::min(cap, pacing.unfocusedFps) : pacing.unfocusedFps;
    if (cap > 0.f && now - lastFrame < 1.0 / cap) d.sleep = 1.0 / cap - (now - lastFrame);
    return d;
}

void Application::requestRedraw(float seconds) { activeUntil_ = std::max(activeUntil_, nowSeconds() + std::max(seconds, 0.f)); }

bool Application::beginFrame(float& dt) {
    const bool paced = options_.screenshotPath.empty();   // (screenshot runs: every frame, as before)
    for (;;) {
        if (quit_ || platform::closeRequested()) return false;
        platform::pollEvents();
        platform::windowSize(width_, height_);
        if (float scale = std::max(platform::dpiScale(), 1.f); std::fabs(scale - uiScale_) > 0.01f) {   // (another monitor)
            uiScale_ = scale;
            applyStyle(scale);
        }
        renderer_.resize(width_, height_);

        double now = nowSeconds();
        // Something happening: input waiting for Dear ImGui, the window resized, a key or button held, the renderer
        // still settling
        ImGuiContext& g = *ImGui::GetCurrentContext();
        bool held = ImGui::IsAnyMouseDown();
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !held; ++k) held = g.IO.KeysData[k - ImGuiKey_NamedKey_BEGIN].Down;
        if (g.InputEventsQueue.Size > 0 || width_ != lastWidth_ || height_ != lastHeight_ || held || renderer_.settling())
            activeUntil_ = std::max(activeUntil_, now + pacing_.settleSeconds);
        lastWidth_ = width_;
        lastHeight_ = height_;
        if (width_ == 0 || height_ == 0) {   // minimised: nothing to draw, wait for the window to come back
            platform::waitEvents(0.25);
            continue;
        }
        Decision d = paced ? decide(pacing_, now, activeUntil_, lastFrame_, platform::windowFocused()) : Decision{};
        if (!d.draw) {   // idle: sleep until an event (or the heartbeat)
            if (platform::waitEvents(d.wait)) activeUntil_ = nowSeconds() + pacing_.settleSeconds;
            continue;
        }
        idleFrame_ = d.idle;
        if (d.sleep > 0.0) {   // (the frame-rate cap)
            std::this_thread::sleep_for(std::chrono::duration<double>(d.sleep));
            now = nowSeconds();
        }
        lastFrame_ = now;
        ++fpsFrames_;
        if (now - fpsWindow_ >= 1.0) {
            fps_ = fpsFrames_ / (now - fpsWindow_);
            fpsFrames_ = 0;
            fpsWindow_ = now;
        }
        dt = (float)std::min(now - lastTime_, 0.1);
        lastTime_ = now;
        profiler::endFrame();   // (the previous one)
        profiler::beginFrame();
        gamefonts::loadPending();   // (fonts the game asked for last frame: they can only be added between frames)
        ImGui_ImplVulkan_NewFrame();
        platform::imguiNewFrame(dt);
        ImGui::NewFrame();
        return true;
    }
}

void Application::endFrame(const SceneRenderInfo& scene) {
    {
        PROFILE_SCOPE("UI layout");
        ImGui::Render();
    }
    if (!options_.screenshotPath.empty() && ++frameCount_ == options_.screenshotFrame)
        renderer_.requestScreenshot(options_.screenshotPath);
    renderer_.render(scene, ImGui::GetDrawData());
    if (renderer_.screenshotDone() != 0) {   // screenshot mode: done
        screenshotResult_ = renderer_.screenshotDone();
        quit_ = true;
    }
}

int Application::exitCode() const {
    if (!options_.screenshotPath.empty() && screenshotResult_ < 0) return 1;
    if (Renderer::validationErrors() > 0) {
        std::fprintf(stderr, "%d validation errors\n", Renderer::validationErrors());
        return 1;
    }
    return 0;
}

GameUI::TextureLookup Application::uiTextures(TextureLibrary& textures) {
    auto failed = std::make_shared<std::set<std::string>>();   // files that didn't load: don't retry every frame
    Renderer* renderer = &renderer_;
    return [&textures, renderer, failed](const std::string& name) -> ImTextureID {
        int id = textures.find(name);
        if (id < 0 && !failed->count(name)) {
            std::string error;
            id = loadTexture(textures, project::resolve(name), error);
            if (id < 0) {
                failed->insert(name);
                std::fprintf(stderr, "UI image '%s': %s\n", name.c_str(), error.c_str());
            }
        }
        return id >= 0 ? renderer->uiTexture(id) : (ImTextureID)0;
    };
}
