#include "app/application.h"
#include "core/profiler.h"

#include <algorithm>
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
#include "platform/platform.h"

namespace {

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void setupImGuiStyle(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // fixed layout, nothing worth persisting
    for (const auto& font : platform::uiFontCandidates()) {
        std::error_code ec;
        if (std::filesystem::exists(font, ec) && io.Fonts->AddFontFromFileTTF(font.u8string().c_str(), 16.f * scale))
            break;
    }

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
}

}  // namespace

Application::Options Application::parseArgs(int& argc, char** argv, Options o) {
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fullscreen")) {
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

bool Application::beginFrame(float& dt) {
    for (;;) {
        if (quit_ || platform::closeRequested()) return false;
        platform::pollEvents();
        platform::windowSize(width_, height_);
        renderer_.resize(width_, height_);

        double now = nowSeconds();
        dt = (float)std::min(now - lastTime_, 0.1);
        lastTime_ = now;
        if (width_ == 0 || height_ == 0) {   // minimised
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        profiler::endFrame();   // (the previous one)
        profiler::beginFrame();
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
