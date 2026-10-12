// Native window, input and OS services. One implementation per OS:
//   platform_xcb.cpp   (Linux: X11 / XWayland via XCB)
//   platform_win32.cpp (Windows)
//   platform_macos.mm  (macOS: Cocoa + CAMetalLayer, Vulkan via MoltenVK)
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>
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
// The window's icon (title bar, task bar, dock): RGBA8 pixels, rows top to bottom
void setWindowIcon(const uint8_t* rgba, uint32_t width, uint32_t height);
// Windows: makes `ico` (an .ico file's bytes) the icon of the executable `exe` (Explorer, the task bar), as its icon
// resources. Elsewhere: false (executables there have no icons of their own).
bool setExecutableIcon(const std::filesystem::path& exe, const std::string& ico, std::string& error);
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

// Native "open file" dialog filtered to extensions (comma-separated, e.g. "obj,gltf,glb"); blocks until closed.
// Returns an empty path when cancelled or when no dialog is available (Linux without zenity/kdialog).
std::filesystem::path openFileDialog(const char* title, const char* extension);
std::filesystem::path openFolderDialog(const char* title);   // empty when cancelled / unavailable

std::filesystem::path executableDir();
std::filesystem::path executablePath();   // empty if unknown
std::vector<std::filesystem::path> uiFontCandidates();

}  // namespace platform
