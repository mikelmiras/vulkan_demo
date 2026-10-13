// Linux: two window backends, picked when the window is made (platform_linux.cpp holds the public functions):
//   platform_xcb.cpp      X11, and XWayland on Wayland sessions
//   platform_wayland.cpp  native Wayland (built when its headers are there: VKWORLD_WAYLAND); used when the
//                         compositor decorates windows itself, or with VKWORLD_PLATFORM=wayland
// and what they share (gamepads, in platform_linux.cpp).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// The window functions of platform.h, once per backend
#define VKWORLD_LINUX_WINDOW_FUNCTIONS                                   \
    bool createWindow(const char* title, uint32_t width, uint32_t height); \
    void destroyWindow();                                                \
    void pollEvents();                                                   \
    bool waitEvents(double seconds);                                     \
    bool windowFocused();                                                \
    bool closeRequested();                                               \
    void windowSize(uint32_t& width, uint32_t& height);                  \
    void setCursorCaptured(bool captured);                               \
    bool cursorCaptured();                                               \
    void takeMouseDelta(float& dx, float& dy);                           \
    void setFullscreen(bool fullscreen);                                 \
    bool isFullscreen();                                                 \
    void setWindowIcon(const uint8_t* rgba, uint32_t width, uint32_t height); \
    float dpiScale();                                                    \
    const char* vulkanSurfaceExtension();                                \
    VkSurfaceKHR createVulkanSurface(VkInstance instance);               \
    void imguiInit();                                                    \
    void imguiNewFrame(float deltaTime);                                 \
    void imguiShutdown();

namespace platform::x11 {
VKWORLD_LINUX_WINDOW_FUNCTIONS
}

namespace platform::wl {
VKWORLD_LINUX_WINDOW_FUNCTIONS
// Whether to try it: a Wayland session (WAYLAND_DISPLAY) unless VKWORLD_PLATFORM=x11. createWindow() also says
// no when the compositor doesn't decorate windows (GNOME) and XWayland is there to do it, unless forced.
bool wanted();
}

namespace platform::linux_shared {

// Gamepads: the Linux joystick API (/dev/input/js*) for input, its evdev node for rumble, sysfs for light bars.
// open() once ImGui is set up; poll() every frame feeds ImGui and opens a pad plugged in later.
void gamepadOpen();
void gamepadPoll();
void gamepadClose();

}  // namespace platform::linux_shared
