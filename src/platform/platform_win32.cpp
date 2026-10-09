// Windows platform layer: Win32 window + Dear ImGui's official Win32 backend.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "platform/platform.h"

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
bool g_captured = false;
float g_mouseDx = 0.f, g_mouseDy = 0.f;
bool g_fullscreen = false;
WINDOWPLACEMENT g_windowedPlacement{sizeof(WINDOWPLACEMENT)};

// Keep the (hidden) cursor inside the client area while captured
void clipCursorToWindow() {
    RECT r;
    GetClientRect(g_hwnd, &r);
    POINT tl{r.left, r.top}, br{r.right, r.bottom};
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    RECT clip{tl.x, tl.y, br.x, br.y};
    ClipCursor(&clip);
    SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
}

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
        case WM_INPUT: {   // raw mouse movement (registered in createWindow), used while captured
            if (!g_captured) break;
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouseDx += (float)raw.data.mouse.lLastX;
                g_mouseDy += (float)raw.data.mouse.lLastY;
            }
            break;
        }
        case WM_KILLFOCUS:
            setCursorCaptured(false);
            break;
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

    RAWINPUTDEVICE mouse{0x01, 0x02, 0, g_hwnd};   // generic desktop page, mouse
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    RECT client{};
    GetClientRect(g_hwnd, &client);
    g_width = (uint32_t)(client.right - client.left);
    g_height = (uint32_t)(client.bottom - client.top);
    return true;
}

void destroyWindow() {
    setCursorCaptured(false);
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

void setCursorCaptured(bool captured) {
    if (captured == g_captured || !g_hwnd) return;
    g_captured = captured;
    g_mouseDx = g_mouseDy = 0.f;
    if (captured) {
        clipCursorToWindow();
        while (ShowCursor(FALSE) >= 0) {}   // the display count is a counter: hide until it's negative
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

bool cursorCaptured() { return g_captured; }

void takeMouseDelta(float& dx, float& dy) {
    dx = g_mouseDx;
    dy = g_mouseDy;
    g_mouseDx = g_mouseDy = 0.f;
}

// Borderless window covering the monitor, the usual "fullscreen windowed" mode
void setFullscreen(bool fullscreen) {
    if (fullscreen == g_fullscreen || !g_hwnd) return;
    g_fullscreen = fullscreen;
    if (fullscreen) {
        GetWindowPlacement(g_hwnd, &g_windowedPlacement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &g_windowedPlacement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    if (g_captured) clipCursorToWindow();
}

bool isFullscreen() { return g_fullscreen; }

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

std::filesystem::path openFileDialog(const char* title, const char* extension) {
    std::wstring exts = widen(extension), wtitle = widen(title), patterns;
    for (size_t start = 0; start <= exts.size();) {   // "obj,glb" -> "*.obj;*.glb"
        size_t comma = exts.find(L',', start);
        if (comma == std::wstring::npos) comma = exts.size();
        if (comma > start) patterns += (patterns.empty() ? L"*." : L";*.") + exts.substr(start, comma - start);
        start = comma + 1;
    }
    // Filter: "Supported files (*.obj;*.glb)\0*.obj;*.glb\0All files\0*.*\0\0"
    std::wstring filter = L"Supported files (" + patterns + L")" + L'\0' + patterns + L'\0' + L"All files" + L'\0' +
                          L"*.*" + L'\0';
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return {};
    return std::filesystem::path(file);
}

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
