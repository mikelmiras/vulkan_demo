// Windows platform layer: Win32 window + Dear ImGui's official Win32 backend.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <xinput.h>
#include <winhttp.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "platform/platform.h"

#include <algorithm>
#include <cstring>
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

// XInput, loaded when first needed (as Dear ImGui's backend does for input): rumble on the first pad connected
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_xinputGetState = nullptr;
XInputSetStateFn g_xinputSetState = nullptr;
bool g_xinputTried = false;
ULONGLONG g_rumbleUntil[XUSER_MAX_COUNT] = {};   // (XInput has no duration: each pad's stopped by pollEvents)

bool loadXInput() {
    if (g_xinputTried) return g_xinputSetState != nullptr;
    g_xinputTried = true;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"})
        if (HMODULE m = LoadLibraryA(dll)) {
            g_xinputGetState = (XInputGetStateFn)(void*)GetProcAddress(m, "XInputGetState");
            g_xinputSetState = (XInputSetStateFn)(void*)GetProcAddress(m, "XInputSetState");
            break;
        }
    return g_xinputGetState && g_xinputSetState;
}

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
            // Follow the suggested size when moved to a monitor with a different DPI (the UI rescales to it)
            g_dpiScale = (float)HIWORD(wParam) / 96.f;
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
    for (int i = 0; i < XUSER_MAX_COUNT; ++i)
        if (g_rumbleUntil[i]) gamepadRumble(i, 0.f, 0.f, 0.f);
    DestroyWindow(g_hwnd);
    UnregisterClassW(kClassName, g_instance);
    g_hwnd = nullptr;
}

void pollEvents() {
    for (int i = 0; i < XUSER_MAX_COUNT; ++i)
        if (g_rumbleUntil[i] && GetTickCount64() >= g_rumbleUntil[i]) gamepadRumble(i, 0.f, 0.f, 0.f);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) g_closeRequested = true;
    }
}

bool waitEvents(double seconds) {
    DWORD ms = (DWORD)std::clamp(seconds * 1000.0, 0.0, 60000.0);
    return MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT) == WAIT_OBJECT_0;
}

bool windowFocused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

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

bool gamepadRumble(float low, float high, float seconds) { return gamepadRumble(-1, low, high, seconds); }

bool gamepadRumble(int pad, float low, float high, float seconds) {
    if (!loadXInput()) return false;
    bool stop = seconds <= 0.f || (low <= 0.f && high <= 0.f);
    if (pad < 0) {   // the first one connected (stopping: every one rumbling)
        if (stop) {
            bool ok = true;
            for (int i = 0; i < XUSER_MAX_COUNT; ++i)
                if (g_rumbleUntil[i]) ok = gamepadRumble(i, 0.f, 0.f, 0.f) && ok;
            return ok;
        }
        for (DWORD i = 0; i < XUSER_MAX_COUNT && pad < 0; ++i) {
            XINPUT_STATE state{};
            if (g_xinputGetState(i, &state) == ERROR_SUCCESS) pad = (int)i;
        }
    }
    if (pad < 0 || pad >= XUSER_MAX_COUNT) return false;
    XINPUT_VIBRATION v{};
    if (!stop) {   // (left: the heavy motor, right: the light one)
        v.wLeftMotorSpeed = (WORD)(std::clamp(low, 0.f, 1.f) * 65535.f);
        v.wRightMotorSpeed = (WORD)(std::clamp(high, 0.f, 1.f) * 65535.f);
    }
    g_rumbleUntil[pad] = stop ? 0 : GetTickCount64() + (ULONGLONG)(seconds * 1000.f);
    return g_xinputSetState((DWORD)pad, &v) == ERROR_SUCCESS;
}

// XInput's four pads, one by one (Dear ImGui's backend gives it the first connected; all Xbox-like)
std::vector<GamepadState> gamepads() {
    std::vector<GamepadState> out(XUSER_MAX_COUNT);
    if (!loadXInput()) return out;
    static ULONGLONG retry[XUSER_MAX_COUNT] = {};   // (asking an empty slot is slow: those only every 2 s)
    ULONGLONG now = GetTickCount64();
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (retry[i] > now) continue;
        XINPUT_STATE st{};
        if (g_xinputGetState(i, &st) != ERROR_SUCCESS) {
            retry[i] = now + 2000;
            continue;
        }
        retry[i] = 0;
        GamepadState& p = out[i];
        p.connected = true;
        p.type = PadType::Xbox;
        p.name = "XInput pad " + std::to_string(i + 1);
        const XINPUT_GAMEPAD& g = st.Gamepad;
        auto button = [&](ImGuiKey k, WORD bit) { p.set(k, (g.wButtons & bit) ? 1.f : 0.f); };
        button(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
        button(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
        button(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
        button(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
        button(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
        button(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
        button(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
        button(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
        button(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
        button(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
        button(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
        button(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        button(ImGuiKey_GamepadL3, XINPUT_GAMEPAD_LEFT_THUMB);
        button(ImGuiKey_GamepadR3, XINPUT_GAMEPAD_RIGHT_THUMB);
        auto trigger = [](BYTE v) { return v <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0.f : (v - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) / (255.f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD); };
        p.set(ImGuiKey_GamepadL2, trigger(g.bLeftTrigger));
        p.set(ImGuiKey_GamepadR2, trigger(g.bRightTrigger));
        auto stick = [&](ImGuiKey negative, ImGuiKey positive, SHORT raw, bool flip) {
            float v = (flip ? -raw : raw) / 32767.f, dead = (float)XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.f;
            p.set(negative, std::clamp((-v - dead) / (1.f - dead), 0.f, 1.f));
            p.set(positive, std::clamp((v - dead) / (1.f - dead), 0.f, 1.f));
        };
        stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, g.sThumbLX, false);
        stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, g.sThumbLY, true);   // (XInput's y is up)
        stick(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, g.sThumbRX, false);
        stick(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, g.sThumbRY, true);
    }
    return out;
}

bool setGamepadLight(float, float, float) { return false; }   // (XInput has no light bars)

// The .ico's images become RT_ICON resources 1..n and its directory the RT_GROUP_ICON 1 (the one Explorer shows)
bool setExecutableIcon(const std::filesystem::path& exe, const std::string& ico, std::string& error) {
#pragma pack(push, 2)
    struct IcoHeader { WORD reserved, type, count; };
    struct IcoEntry { BYTE width, height, colors, reserved; WORD planes, bits; DWORD bytes, offset; };
    struct GroupEntry { BYTE width, height, colors, reserved; WORD planes, bits; DWORD bytes; WORD id; };
#pragma pack(pop)
    if (ico.size() < sizeof(IcoHeader)) { error = "not an icon"; return false; }
    IcoHeader h;
    std::memcpy(&h, ico.data(), sizeof(h));
    if (h.type != 1 || h.count == 0 || ico.size() < sizeof(IcoHeader) + h.count * sizeof(IcoEntry)) { error = "not an icon"; return false; }
    HANDLE update = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
    if (!update) { error = "cannot open " + exe.u8string() + " for its resources"; return false; }
    const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    std::string group(sizeof(IcoHeader) + h.count * sizeof(GroupEntry), '\0');
    std::memcpy(group.data(), &h, sizeof(h));
    bool ok = true;
    for (WORD i = 0; i < h.count && ok; ++i) {
        IcoEntry e;
        std::memcpy(&e, ico.data() + sizeof(IcoHeader) + i * sizeof(IcoEntry), sizeof(e));
        if ((size_t)e.offset + e.bytes > ico.size()) { ok = false; break; }
        ok = UpdateResourceW(update, MAKEINTRESOURCEW(3) /* RT_ICON */, MAKEINTRESOURCEW(i + 1), lang,
                             (void*)(ico.data() + e.offset), e.bytes) != FALSE;
        GroupEntry g{e.width, e.height, e.colors, 0, e.planes, e.bits, e.bytes, (WORD)(i + 1)};
        std::memcpy(group.data() + sizeof(IcoHeader) + i * sizeof(GroupEntry), &g, sizeof(g));
    }
    ok = ok && UpdateResourceW(update, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, MAKEINTRESOURCEW(1), lang,
                               group.data(), (DWORD)group.size()) != FALSE;
    if (!EndUpdateResourceW(update, ok ? FALSE : TRUE) || !ok) {
        error = "cannot write the icon into " + exe.u8string();
        return false;
    }
    return true;
}

// A 32-bit icon from the pixels (BGRA, bottom-up DIB), for the title bar and the task bar
void setWindowIcon(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!g_hwnd || !rgba || !width || !height) return;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = (LONG)width;
    bi.bV5Height = -(LONG)height;   // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return;
    uint8_t* dst = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < (size_t)width * height; ++i) {
        dst[i * 4 + 0] = rgba[i * 4 + 2];
        dst[i * 4 + 1] = rgba[i * 4 + 1];
        dst[i * 4 + 2] = rgba[i * 4 + 0];
        dst[i * 4 + 3] = rgba[i * 4 + 3];
    }
    HBITMAP mask = CreateBitmap((int)width, (int)height, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!icon) return;
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
}

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

std::filesystem::path openFolderDialog(const char* title) {
    std::wstring wtitle = widen(title);
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&bi);
    if (!item) return {};
    wchar_t path[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    return ok ? std::filesystem::path(path) : std::filesystem::path();
}

bool openWithSystem(const std::filesystem::path& file) {
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// CreateProcess with a pipe for its output; arguments quoted the way the C runtime parses them back
static std::wstring commandLine(const std::vector<std::string>& args) {
    std::wstring line;
    for (const std::string& a : args) {
        std::wstring w = widen(a.c_str()), q = L"\"";
        for (size_t i = 0; i < w.size(); ++i) {
            size_t slashes = 0;
            while (i < w.size() && w[i] == L'\\') ++slashes, ++i;
            if (i == w.size()) { q.append(slashes * 2, L'\\'); break; }
            if (w[i] == L'"') { q.append(slashes * 2 + 1, L'\\'); q += L'"'; }
            else { q.append(slashes, L'\\'); q += w[i]; }
        }
        q += L"\"";
        line += (line.empty() ? L"" : L" ") + q;
    }
    return line;
}

bool startProgram(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    std::wstring line = commandLine(args);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool runProgram(const std::vector<std::string>& args, std::string& output, int& exitCode) {
    output.clear();
    exitCode = -1;
    if (args.empty()) return false;
    std::wstring line = commandLine(args);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return false;
    }
    char buf[4096];
    for (DWORD n = 0; ReadFile(readEnd, buf, sizeof(buf), &n, nullptr) && n > 0;) output.append(buf, n);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCode = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// WinHTTP (the system's TLS and proxy settings)
static bool httpRequest(const std::string& url, const std::string* data, const std::string& contentType, std::string& body,
                        std::string& error, int timeoutSeconds) {
    body.clear();
    std::wstring wurl = widen(url.c_str());
    URL_COMPONENTS parts{sizeof(URL_COMPONENTS)};
    wchar_t host[256] = {0}, path[2048] = {0};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    parts.dwExtraInfoLength = (DWORD)-1;   // (the query string: appended to the path below)
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
        error = "not an http(s) URL: " + url;
        return false;
    }
    std::wstring object = path;
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"vkworld", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = nullptr, request = nullptr;
    if (session) {
        int ms = std::max(timeoutSeconds, 1) * 1000;
        WinHttpSetTimeouts(session, ms, ms, ms, ms);
        connection = WinHttpConnect(session, host, parts.nPort, 0);
    }
    if (connection)
        request = WinHttpOpenRequest(connection, data ? L"POST" : L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    std::wstring headers = data ? widen(("Content-Type: " + contentType).c_str()) : std::wstring();
    bool sent = request && (data ? WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L, (LPVOID)data->data(), (DWORD)data->size(),
                                                      (DWORD)data->size(), 0)
                                 : WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0));
    if (sent && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            body.append(chunk.data(), read);
        }
        ok = status >= 200 && status < 300;
        if (!ok) error = "HTTP " + std::to_string(status) + " from " + url;
    } else {
        error = "can't reach " + url + " (error " + std::to_string(GetLastError()) + ")";
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!ok) body.clear();
    return ok;
}

bool httpGet(const std::string& url, std::string& body, std::string& error, int timeoutSeconds) {
    return httpRequest(url, nullptr, "", body, error, timeoutSeconds);
}

bool httpPost(const std::string& url, const std::string& data, const std::string& contentType, std::string& body,
              std::string& error, int timeoutSeconds) {
    return httpRequest(url, &data, contentType, body, error, timeoutSeconds);
}

std::filesystem::path executablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);   // path longer than the buffer: retry
    }
}

std::filesystem::path executableDir() {
    std::filesystem::path exe = executablePath();
    return exe.empty() ? std::filesystem::current_path() : exe.parent_path();
}

std::vector<std::filesystem::path> uiFontCandidates() {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    std::filesystem::path fonts = std::filesystem::path(n ? dir : L"C:\\Windows") / L"Fonts";
    return {fonts / L"segoeui.ttf", fonts / L"arial.ttf", fonts / L"tahoma.ttf"};
}

}  // namespace platform
