// Windows platform layer: Win32 window + Dear ImGui's official Win32 backend.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "platform.h"

#include <algorithm>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"

// Declared here as recommended by imgui_impl_win32.h (it's commented out there to avoid pulling in windows.h)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace platform {

namespace {

const wchar_t* kClassName = L"VulkanWorldEditor";
HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
uint32_t g_width = 0, g_height = 0;
bool g_closeRequested = false;
float g_dpiScale = 1.f;

std::wstring widen(const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    w.resize((size_t)n - 1);   // drop the terminator written by the API
    return w;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                g_width = g_height = 0;
            } else {
                g_width = LOWORD(lParam);
                g_height = HIWORD(lParam);
            }
            return 0;
        case WM_DPICHANGED: {
            // Follow the suggested size when moved to a monitor with a different DPI
            const RECT* r = (const RECT*)lParam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // Alt alone must not open the window menu (Alt+H etc.)
            break;
        case WM_CLOSE:
            g_closeRequested = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool createWindow(const char* title, uint32_t width, uint32_t height) {
    ImGui_ImplWin32_EnableDpiAwareness();
    g_instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    // Scale the requested size by the monitor DPI, but keep it within 90% of the work area
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    float w = width * g_dpiScale, h = height * g_dpiScale;
    float maxW = 0.9f * (float)(mi.rcWork.right - mi.rcWork.left), maxH = 0.9f * (float)(mi.rcWork.bottom - mi.rcWork.top);
    float fit = std::min({1.f, maxW / w, maxH / h});
    RECT rect{0, 0, (LONG)(w * fit), (LONG)(h * fit)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, g_instance, nullptr);
    if (!g_hwnd) return false;
    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    RECT client{};
    GetClientRect(g_hwnd, &client);
    g_width = (uint32_t)(client.right - client.left);
    g_height = (uint32_t)(client.bottom - client.top);
    return true;
}

void destroyWindow() {
    DestroyWindow(g_hwnd);
    UnregisterClassW(kClassName, g_instance);
    g_hwnd = nullptr;
}

void pollEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) g_closeRequested = true;
    }
}

bool closeRequested() { return g_closeRequested; }

void windowSize(uint32_t& width, uint32_t& height) {
    width = g_width;
    height = g_height;
}

float dpiScale() { return g_dpiScale; }

const char* vulkanSurfaceExtension() { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

VkSurfaceKHR createVulkanSurface(VkInstance instance) {
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = g_instance;
    ci.hwnd = g_hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
    return surface;
}

void imguiInit() { ImGui_ImplWin32_Init(g_hwnd); }

void imguiNewFrame(float) { ImGui_ImplWin32_NewFrame(); }   // the backend measures size and time itself

void imguiShutdown() { ImGui_ImplWin32_Shutdown(); }

std::filesystem::path executableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::filesystem::current_path();
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);   // path longer than the buffer: retry
    }
}

std::vector<std::filesystem::path> uiFontCandidates() {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    std::filesystem::path fonts = std::filesystem::path(n ? dir : L"C:\\Windows") / L"Fonts";
    return {fonts / L"segoeui.ttf", fonts / L"arial.ttf", fonts / L"tahoma.ttf"};
}

}  // namespace platform
