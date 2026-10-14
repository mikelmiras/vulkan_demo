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

#include "scene/input_bindings.h"

namespace platform {

bool createWindow(const char* title, uint32_t width, uint32_t height);
void destroyWindow();

// Processes pending OS events and feeds input to ImGui (once the ImGui backend is initialised).
void pollEvents();
// Sleeps until the window gets an event (input, a resize, a close) or `seconds` pass; true if one came. pollEvents()
// then takes it. (An editor with nothing to do waits here instead of drawing frames nobody needs.)
bool waitEvents(double seconds);
bool windowFocused();   // the window has the keyboard (in front)
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
// Windows: `resource` (a VS_VERSIONINFO: app/exe_metadata.h) as the version resource of the executable `exe`.
// Elsewhere: false (no such resources).
bool setExecutableVersion(const std::filesystem::path& exe, const std::string& resource, std::string& error);
bool isFullscreen();

// Gamepad feedback, on the first pad: rumble with the heavy (low frequency) and light (high frequency) motors 0..1
// for `seconds` (0 stops it), and a light bar's colour (DualShock 4 / DualSense, 0..1). False where the pad, the
// OS or permissions don't allow it (Windows: light bars on Sony's pads read as HID; Linux: the LEDs are often root's).
bool gamepadRumble(float low, float high, float seconds);
bool setGamepadLight(float r, float g, float b);
// Gamepads one by one: slots (a pad keeps its slot while it's connected), what each has down and what kind it is.
// Dear ImGui still gets them all together (menus answer any pad). macOS: the GameController framework's; Windows:
// XInput's four, then four for DualShock 4s, DualSenses and Switch Pro Controllers read as HID (platform/hid_pads.h).
std::vector<GamepadState> gamepads();
bool gamepadRumble(int pad, float low, float high, float seconds);   // a pad's own (its slot; -1: the first)

// Content scale of the window's monitor (1.0 = 96 DPI on Windows, always 1.0 on X11)
float dpiScale();
// How bright the window's display can go (nits; false if it doesn't say): DXGI's output description on Windows,
// the EDID's HDR metadata on Linux (the brightest connected display: which one shows the window isn't known), the
// screen's extended dynamic range headroom on macOS (times 100 nits)
bool displayLuminance(float& peak, float& frameAverage, float& min);

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
// Opens a file in the program the system uses for it (xdg-open, ShellExecute, open); false if that can't start
bool openWithSystem(const std::filesystem::path& file);   // (a URL too)

// Runs a program (looked up on the PATH unless args[0] is a path) with these arguments and waits for it: false if it
// couldn't start. `output`: what it wrote (stdout and stderr), `exitCode` how it ended. (Export packaging tools.)
bool runProgram(const std::vector<std::string>& args, std::string& output, int& exitCode);
// Starts a program and goes on without waiting for it (a game restarting itself after an update)
bool startProgram(const std::vector<std::string>& args);

// Fetches a URL (http or https, through the OS's own TLS: WinHTTP, NSURLSession; curl or wget on Linux), waiting
// up to `timeoutSeconds`: the body of a successful answer, else false and why. Blocks: call it on a worker thread.
bool httpGet(const std::string& url, std::string& body, std::string& error, int timeoutSeconds = 10);
// The same, sending `data` (POST, as `contentType`): the answer's body in `body` (crash reports)
bool httpPost(const std::string& url, const std::string& data, const std::string& contentType, std::string& body,
              std::string& error, int timeoutSeconds = 10);

std::filesystem::path executableDir();
std::filesystem::path executablePath();   // empty if unknown
std::vector<std::filesystem::path> uiFontCandidates();

}  // namespace platform
