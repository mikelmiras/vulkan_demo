// Native window, input and OS services. One implementation per OS:
//   platform_xcb.cpp   (Linux: X11 / XWayland via XCB)
//   platform_win32.cpp (Windows)
//   platform_macos.mm  (macOS: Cocoa + CAMetalLayer, Vulkan via MoltenVK)
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace platform {

bool createWindow(const char* title, uint32_t width, uint32_t height);
void destroyWindow();

// Processes pending OS events and feeds input to ImGui (once the ImGui backend is initialised).
void pollEvents();
bool closeRequested();
void windowSize(uint32_t& width, uint32_t& height);   // client area in pixels; 0 when minimised

// Mouse capture for first-person games: hides the cursor, keeps it inside the window and reports raw movement.
// Released automatically when the window loses focus.
void setCursorCaptured(bool captured);
bool cursorCaptured();
void takeMouseDelta(float& dx, float& dy);   // movement since the previous call while captured, in pixels; else 0

void setFullscreen(bool fullscreen);
bool isFullscreen();

// Content scale of the window's monitor (1.0 = 96 DPI on Windows, always 1.0 on X11)
float dpiScale();

// Vulkan
const char* vulkanSurfaceExtension();
VkSurfaceKHR createVulkanSurface(VkInstance instance);

// Dear ImGui platform backend
void imguiInit();
void imguiNewFrame(float deltaTime);
void imguiShutdown();

// Native "open file" dialog filtered to one extension (e.g. "obj"); blocks until closed.
// Returns an empty path when cancelled or when no dialog is available (Linux without zenity/kdialog).
std::filesystem::path openFileDialog(const char* title, const char* extension);

std::filesystem::path executableDir();
std::vector<std::filesystem::path> uiFontCandidates();

}  // namespace platform
