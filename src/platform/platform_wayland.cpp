// Linux window backend: native Wayland (platform_linux.h). xdg-shell for the window, server-side decorations
// (xdg-decoration) where the compositor offers them, xkbcommon for the keyboard, wayland-cursor for the pointer's
// look, and a locked pointer with relative motion (pointer-constraints, relative-pointer) for mouse capture. HiDPI: the
// window is drawn in physical pixels (like Windows), its scale from wp_fractional_scale_v1 where the compositor has it
// (else the outputs it's on), shown at its logical size through wp_viewporter; the UI scales with dpiScale().
#include "platform/wayland_api.h"   // (first: the libraries' functions are loaded at run time)

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "platform/platform.h"
#include "platform/platform_linux.h"
#include "platform/imgui_impl_xcb.h"   // (the keysym table: xkb keysyms are X's)

#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"

namespace platform::wl {

namespace {

struct State {
    bool focused = true;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    xdg_wm_base* wmBase = nullptr;
    zxdg_decoration_manager_v1* decorationManager = nullptr;
    zwp_relative_pointer_manager_v1* relativeManager = nullptr;
    zwp_pointer_constraints_v1* constraints = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractionalManager = nullptr;
    struct Output {
        wl_output* output;
        uint32_t name;
        int scale;
    };
    std::vector<Output> outputs;
    std::vector<wl_output*> on;          // the outputs the window is on

    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zxdg_toplevel_decoration_v1* decoration = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractional = nullptr;
    bool configured = false, closeRequested = false, fullscreen = false;
    uint32_t width = 0, height = 0;      // logical (surface) size
    double scale = 1.0;                  // physical pixels per logical one (with a viewport; else 1)
    bool fractionalScale = false;        // (the compositor says it: outputs' integer scales aren't used)

    // Pointer
    uint32_t enterSerial = 0;
    bool pointerInside = false;
    wl_cursor_theme* cursorTheme = nullptr;
    int cursorScale = 1;
    wl_surface* cursorSurface = nullptr;
    ImGuiMouseCursor cursorShown = ImGuiMouseCursor_COUNT;
    bool captured = false;
    zwp_locked_pointer_v1* locked = nullptr;
    zwp_relative_pointer_v1* relative = nullptr;
    double mouseDx = 0.0, mouseDy = 0.0;

    // Keyboard
    xkb_context* xkb = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* xkbState = nullptr;
    int32_t repeatRate = 25, repeatDelay = 600;   // (keys per second, milliseconds)
    uint32_t repeatKey = 0;                        // the key held for repeat (0: none)
    double repeatAt = 0.0;

    bool imguiReady = false;
};
State g;

// ---------------------------------------------------------------------------------------------
// Scale (HiDPI)
// ---------------------------------------------------------------------------------------------
uint32_t toPixels(uint32_t logical) { return (uint32_t)std::max<long>(std::lround(logical * g.scale), 1); }

// The buffer (any size: physical pixels) shown at the window's logical size
void applyViewport() {
    if (g.viewport && g.width && g.height) wp_viewport_set_destination(g.viewport, (int32_t)g.width, (int32_t)g.height);
}

// Without a fractional scale from the compositor: the largest integer scale of the outputs the window is on
void outputsChanged() {
    if (g.fractionalScale || !g.viewport) return;
    int s = 1;
    for (wl_output* o : g.on)
        for (const State::Output& out : g.outputs)
            if (out.output == o) s = std::max(s, out.scale);
    g.scale = s;
}

void outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void outputDone(void*, wl_output*) {}
void outputScale(void*, wl_output* o, int32_t factor) {
    for (State::Output& out : g.outputs)
        if (out.output == o) out.scale = std::max(factor, 1);
    outputsChanged();
}
const wl_output_listener kOutput = {outputGeometry, outputMode, outputDone, outputScale};

void surfaceEnter(void*, wl_surface*, wl_output* o) {
    g.on.push_back(o);
    outputsChanged();
}
void surfaceLeave(void*, wl_surface*, wl_output* o) {
    g.on.erase(std::remove(g.on.begin(), g.on.end(), o), g.on.end());
    outputsChanged();
}
const wl_surface_listener kSurface = {surfaceEnter, surfaceLeave};

void preferredScale(void*, wp_fractional_scale_v1*, uint32_t scale120) {
    g.fractionalScale = true;
    g.scale = std::max(scale120, 30u) / 120.0;
}
const wp_fractional_scale_v1_listener kFractional = {preferredScale};

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------------------------------------
// Shows `cursor` (a name in the cursor theme; several to try) over the window, or hides it
void showCursor(ImGuiMouseCursor which) {
    if (!g.pointer || !g.pointerInside) return;
    g.cursorShown = which;
    if (which == ImGuiMouseCursor_None || g.captured) {
        wl_pointer_set_cursor(g.pointer, g.enterSerial, nullptr, 0, 0);
        return;
    }
    if (!api::cursorLoaded()) return;   // (no libwayland-cursor: the compositor's default pointer)
    int cursorScale = std::max(1, (int)std::ceil(g.scale - 0.01));   // (drawn at the next whole scale up)
    if (g.cursorTheme && cursorScale != g.cursorScale) {
        wl_cursor_theme_destroy(g.cursorTheme);
        g.cursorTheme = nullptr;
    }
    if (!g.cursorTheme) {
        const char* size = std::getenv("XCURSOR_SIZE");
        g.cursorTheme = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), (size ? std::atoi(size) : 24) * cursorScale, g.shm);
        if (!g.cursorTheme) return;
        g.cursorScale = cursorScale;
        if (!g.cursorSurface) g.cursorSurface = wl_compositor_create_surface(g.compositor);
    }
    static const char* const names[ImGuiMouseCursor_COUNT][3] = {
        {"default", "left_ptr", nullptr},              // Arrow
        {"text", "xterm", nullptr},                    // TextInput
        {"all-scroll", "fleur", "grabbing"},           // ResizeAll
        {"ns-resize", "sb_v_double_arrow", nullptr},   // ResizeNS
        {"ew-resize", "sb_h_double_arrow", nullptr},   // ResizeEW
        {"nesw-resize", "fd_double_arrow", nullptr},   // ResizeNESW
        {"nwse-resize", "bd_double_arrow", nullptr},   // ResizeNWSE
        {"pointer", "hand2", nullptr},                 // Hand
        {"wait", "watch", nullptr},                    // Wait
        {"progress", "left_ptr_watch", nullptr},       // Progress
        {"not-allowed", "crossed_circle", nullptr},    // NotAllowed
    };
    wl_cursor* cursor = nullptr;
    int index = which >= 0 && which < ImGuiMouseCursor_COUNT ? which : 0;
    for (const char* name : names[index])
        if (name && (cursor = wl_cursor_theme_get_cursor(g.cursorTheme, name))) break;
    if (!cursor) cursor = wl_cursor_theme_get_cursor(g.cursorTheme, "left_ptr");
    if (!cursor || cursor->image_count == 0) return;
    wl_cursor_image* image = cursor->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    wl_pointer_set_cursor(g.pointer, g.enterSerial, g.cursorSurface, (int32_t)image->hotspot_x / g.cursorScale,
                          (int32_t)image->hotspot_y / g.cursorScale);
    wl_surface_set_buffer_scale(g.cursorSurface, g.cursorScale);
    wl_surface_attach(g.cursorSurface, buffer, 0, 0);
    wl_surface_damage(g.cursorSurface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(g.cursorSurface);
}

void pointerEnter(void*, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    g.enterSerial = serial;
    g.pointerInside = true;
    showCursor(g.imguiReady ? ImGui::GetMouseCursor() : ImGuiMouseCursor_Arrow);
    if (g.imguiReady)
        ImGui::GetIO().AddMousePosEvent((float)(wl_fixed_to_double(x) * g.scale), (float)(wl_fixed_to_double(y) * g.scale));
}
void pointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) {
    g.pointerInside = false;
    if (g.imguiReady) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}
void pointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    if (g.imguiReady && !g.captured)   // (in pixels, as the window is drawn)
        ImGui::GetIO().AddMousePosEvent((float)(wl_fixed_to_double(x) * g.scale), (float)(wl_fixed_to_double(y) * g.scale));
}
void pointerButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    if (!g.imguiReady) return;
    int b = button == BTN_LEFT ? 0 : button == BTN_RIGHT ? 1 : button == BTN_MIDDLE ? 2 : button == BTN_SIDE ? 3
          : button == BTN_EXTRA ? 4 : -1;
    if (b >= 0) ImGui::GetIO().AddMouseButtonEvent(b, state == WL_POINTER_BUTTON_STATE_PRESSED);
}
void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    if (!g.imguiReady) return;
    float v = (float)wl_fixed_to_double(value) / 10.f;   // (a wheel notch is 10 here; ImGui wants 1, up positive)
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ImGui::GetIO().AddMouseWheelEvent(0.f, -v);
    else ImGui::GetIO().AddMouseWheelEvent(-v, 0.f);
}
void pointerFrame(void*, wl_pointer*) {}
void pointerAxisSource(void*, wl_pointer*, uint32_t) {}
void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener kPointer = {pointerEnter, pointerLeave, pointerMotion, pointerButton, pointerAxis,
                                      pointerFrame, pointerAxisSource, pointerAxisStop, pointerAxisDiscrete};

void relativeMotion(void*, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy,
                    wl_fixed_t, wl_fixed_t) {
    if (!g.captured) return;
    g.mouseDx += wl_fixed_to_double(dx);   // (accelerated like the desktop's pointer, as X11's warping gives)
    g.mouseDy += wl_fixed_to_double(dy);
}
const zwp_relative_pointer_v1_listener kRelative = {relativeMotion};

// ---------------------------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------------------------
void keyboardKeymap(void*, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char* text = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (text == MAP_FAILED) return;
    xkb_keymap* keymap = xkb_keymap_new_from_string(g.xkb, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
    if (!keymap) return;
    xkb_state* state = xkb_state_new(keymap);
    if (!state) {
        xkb_keymap_unref(keymap);
        return;
    }
    if (g.xkbState) xkb_state_unref(g.xkbState);
    if (g.keymap) xkb_keymap_unref(g.keymap);
    g.keymap = keymap;
    g.xkbState = state;
}

// The key's ImGui key (from its unshifted symbol, so Shift+1 is still the 1 key)
ImGuiKey keyOf(uint32_t code) {
    const xkb_keysym_t* syms = nullptr;
    int n = xkb_keymap_key_get_syms_by_level(g.keymap, code, 0, 0, &syms);
    return n > 0 ? (ImGuiKey)ImGui_ImplXcb_KeysymToKey(xkb_keysym_to_lower(syms[0])) : ImGuiKey_None;
}

// What the key types, as the layout and modifiers have it (nothing with Ctrl or Alt held: shortcuts)
void typeKey(uint32_t code) {
    if (xkb_state_mod_name_is_active(g.xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ||
        xkb_state_mod_name_is_active(g.xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        return;
    char text[64];
    if (xkb_state_key_get_utf8(g.xkbState, code, text, sizeof(text)) <= 0) return;
    if ((unsigned char)text[0] < 0x20 || text[0] == 0x7f) return;   // (Enter, Backspace, Escape...: keys, not text)
    ImGui::GetIO().AddInputCharactersUTF8(text);
}

void keyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    g.focused = true;
    if (g.imguiReady) ImGui::GetIO().AddFocusEvent(true);
}
void keyboardLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {
    g.repeatKey = 0;
    g.focused = false;
    if (g.imguiReady) ImGui::GetIO().AddFocusEvent(false);   // (ImGui lets go of every key)
    setCursorCaptured(false);
}
void keyboardKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (!g.imguiReady || !g.xkbState) return;
    uint32_t code = key + 8;   // (evdev -> xkb key codes)
    bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    ImGuiKey k = keyOf(code);
    if (k != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(k, down);
    if (down) {
        typeKey(code);
        if (xkb_keymap_key_repeats(g.keymap, code) && g.repeatRate > 0) {
            g.repeatKey = code;
            g.repeatAt = now() + g.repeatDelay / 1000.0;
        }
    } else if (code == g.repeatKey) {
        g.repeatKey = 0;
    }
}
void keyboardModifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked,
                       uint32_t group) {
    if (!g.xkbState) return;
    xkb_state_update_mask(g.xkbState, depressed, latched, locked, 0, 0, group);
    if (!g.imguiReady) return;
    auto active = [](const char* name) { return xkb_state_mod_name_is_active(g.xkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0; };
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, active(XKB_MOD_NAME_CTRL));
    io.AddKeyEvent(ImGuiMod_Shift, active(XKB_MOD_NAME_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt, active(XKB_MOD_NAME_ALT));
    io.AddKeyEvent(ImGuiMod_Super, active(XKB_MOD_NAME_LOGO));
}
void keyboardRepeatInfo(void*, wl_keyboard*, int32_t rate, int32_t delay) {
    g.repeatRate = rate;
    g.repeatDelay = delay;
}
const wl_keyboard_listener kKeyboard = {keyboardKeymap, keyboardEnter, keyboardLeave, keyboardKey, keyboardModifiers,
                                        keyboardRepeatInfo};

// Repeats: the compositor only says the key is held; text repeats here (ImGui repeats held keys itself)
void repeatKeys() {
    if (!g.repeatKey || !g.imguiReady || !g.xkbState) return;
    double t = now();
    for (int i = 0; i < 8 && t >= g.repeatAt; ++i) {
        typeKey(g.repeatKey);
        g.repeatAt += 1.0 / g.repeatRate;
    }
    if (t >= g.repeatAt) g.repeatAt = t + 1.0 / g.repeatRate;   // (far behind: no burst)
}

// ---------------------------------------------------------------------------------------------
// Seat, shell, registry
// ---------------------------------------------------------------------------------------------
void seatCapabilities(void*, wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &kPointer, nullptr);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g.pointer) {
        wl_pointer_destroy(g.pointer);
        g.pointer = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &kKeyboard, nullptr);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && g.keyboard) {
        wl_keyboard_destroy(g.keyboard);
        g.keyboard = nullptr;
    }
}
void seatName(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeat = {seatCapabilities, seatName};

void wmPing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener kWmBase = {wmPing};

void surfaceConfigure(void*, xdg_surface* surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    g.configured = true;
}
const xdg_surface_listener kXdgSurface = {surfaceConfigure};

void toplevelConfigure(void*, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
    if (width > 0 && height > 0) {   // (0: ours to choose; keep it)
        g.width = (uint32_t)width;
        g.height = (uint32_t)height;
        applyViewport();
    }
    bool fullscreen = false;
    for (uint32_t* s = (uint32_t*)states->data; (char*)s < (char*)states->data + states->size; ++s)
        if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = true;
    g.fullscreen = fullscreen;
}
void toplevelClose(void*, xdg_toplevel*) { g.closeRequested = true; }
void toplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevelCapabilities(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevel = {toplevelConfigure, toplevelClose, toplevelBounds, toplevelCapabilities};

void registryGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto bind = [&](const wl_interface* iface, uint32_t want) {
        return wl_registry_bind(registry, name, iface, version < want ? version : want);
    };
    if (!std::strcmp(interface, wl_compositor_interface.name)) {
        g.compositor = (wl_compositor*)bind(&wl_compositor_interface, 4);
    } else if (!std::strcmp(interface, wl_shm_interface.name)) {
        g.shm = (wl_shm*)bind(&wl_shm_interface, 1);
    } else if (!std::strcmp(interface, wl_seat_interface.name) && !g.seat) {
        g.seat = (wl_seat*)bind(&wl_seat_interface, 5);
        wl_seat_add_listener(g.seat, &kSeat, nullptr);
    } else if (!std::strcmp(interface, xdg_wm_base_interface.name)) {
        g.wmBase = (xdg_wm_base*)bind(&xdg_wm_base_interface, 4);
        xdg_wm_base_add_listener(g.wmBase, &kWmBase, nullptr);
    } else if (!std::strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        g.decorationManager = (zxdg_decoration_manager_v1*)bind(&zxdg_decoration_manager_v1_interface, 1);
    } else if (!std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name)) {
        g.relativeManager = (zwp_relative_pointer_manager_v1*)bind(&zwp_relative_pointer_manager_v1_interface, 1);
    } else if (!std::strcmp(interface, zwp_pointer_constraints_v1_interface.name)) {
        g.constraints = (zwp_pointer_constraints_v1*)bind(&zwp_pointer_constraints_v1_interface, 1);
    } else if (!std::strcmp(interface, wp_viewporter_interface.name)) {
        g.viewporter = (wp_viewporter*)bind(&wp_viewporter_interface, 1);
    } else if (!std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        g.fractionalManager = (wp_fractional_scale_manager_v1*)bind(&wp_fractional_scale_manager_v1_interface, 1);
    } else if (!std::strcmp(interface, wl_output_interface.name) && version >= 2) {
        auto* o = (wl_output*)bind(&wl_output_interface, 2);
        g.outputs.push_back({o, name, 1});
        wl_output_add_listener(o, &kOutput, nullptr);
    }
}
void registryGlobalRemove(void*, wl_registry*, uint32_t name) {
    for (size_t i = 0; i < g.outputs.size(); ++i)
        if (g.outputs[i].name == name) {
            g.on.erase(std::remove(g.on.begin(), g.on.end(), g.outputs[i].output), g.on.end());
            wl_output_destroy(g.outputs[i].output);
            g.outputs.erase(g.outputs.begin() + (long)i);
            outputsChanged();
            break;
        }
}
const wl_registry_listener kRegistry = {registryGlobal, registryGlobalRemove};

void disconnect() {
    if (g.relative) zwp_relative_pointer_v1_destroy(g.relative);
    if (g.locked) zwp_locked_pointer_v1_destroy(g.locked);
    if (g.decoration) zxdg_toplevel_decoration_v1_destroy(g.decoration);
    if (g.fractional) wp_fractional_scale_v1_destroy(g.fractional);
    if (g.viewport) wp_viewport_destroy(g.viewport);
    if (g.toplevel) xdg_toplevel_destroy(g.toplevel);
    if (g.xdgSurface) xdg_surface_destroy(g.xdgSurface);
    if (g.surface) wl_surface_destroy(g.surface);
    if (g.cursorSurface) wl_surface_destroy(g.cursorSurface);
    if (g.cursorTheme) wl_cursor_theme_destroy(g.cursorTheme);
    if (g.pointer) wl_pointer_destroy(g.pointer);
    if (g.keyboard) wl_keyboard_destroy(g.keyboard);
    if (g.xkbState) xkb_state_unref(g.xkbState);
    if (g.keymap) xkb_keymap_unref(g.keymap);
    if (g.xkb) xkb_context_unref(g.xkb);
    if (g.constraints) zwp_pointer_constraints_v1_destroy(g.constraints);
    if (g.fractionalManager) wp_fractional_scale_manager_v1_destroy(g.fractionalManager);
    if (g.viewporter) wp_viewporter_destroy(g.viewporter);
    for (State::Output& o : g.outputs) wl_output_destroy(o.output);
    if (g.relativeManager) zwp_relative_pointer_manager_v1_destroy(g.relativeManager);
    if (g.decorationManager) zxdg_decoration_manager_v1_destroy(g.decorationManager);
    if (g.wmBase) xdg_wm_base_destroy(g.wmBase);
    if (g.seat) wl_seat_destroy(g.seat);
    if (g.shm) wl_shm_destroy(g.shm);
    if (g.compositor) wl_compositor_destroy(g.compositor);
    if (g.registry) wl_registry_destroy(g.registry);
    if (g.display) wl_display_disconnect(g.display);
    g = State{};
}

const char* forced() {
    const char* p = std::getenv("VKWORLD_PLATFORM");
    return p && *p ? p : nullptr;
}

}  // namespace

bool wanted() {
    const char* p = forced();
    if (p && !std::strcmp(p, "x11")) return false;
    return std::getenv("WAYLAND_DISPLAY") || (p && !std::strcmp(p, "wayland"));
}

bool createWindow(const char* title, uint32_t width, uint32_t height) {
    if (!api::load()) {
        std::fprintf(stderr, "Wayland: libwayland-client or libxkbcommon isn't installed; using X11\n");
        return false;
    }
    g.display = wl_display_connect(nullptr);
    if (!g.display) return false;
    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &kRegistry, nullptr);
    wl_display_roundtrip(g.display);   // the globals
    wl_display_roundtrip(g.display);   // (the seat's capabilities)
    if (!g.compositor || !g.wmBase || !g.shm) {
        std::fprintf(stderr, "Wayland: the compositor lacks xdg-shell; using X11\n");
        disconnect();
        return false;
    }
    // Without server-side decorations the window would have no title bar to move it by: XWayland's windows get
    // one (from the compositor's X window manager), so that's better when it's there
    bool force = forced() && !std::strcmp(forced(), "wayland");
    if (!g.decorationManager && !force && std::getenv("DISPLAY")) {
        std::fprintf(stderr, "Wayland: no server-side decorations here; using XWayland (VKWORLD_PLATFORM=wayland to force)\n");
        disconnect();
        return false;
    }
    g.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    g.width = width;
    g.height = height;
    g.surface = wl_compositor_create_surface(g.compositor);
    wl_surface_add_listener(g.surface, &kSurface, nullptr);
    if (g.viewporter) {   // (HiDPI: the buffer in pixels, shown at the logical size)
        g.viewport = wp_viewporter_get_viewport(g.viewporter, g.surface);
        if (g.fractionalManager) {
            g.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(g.fractionalManager, g.surface);
            wp_fractional_scale_v1_add_listener(g.fractional, &kFractional, nullptr);
        }
        applyViewport();
    }
    g.xdgSurface = xdg_wm_base_get_xdg_surface(g.wmBase, g.surface);
    xdg_surface_add_listener(g.xdgSurface, &kXdgSurface, nullptr);
    g.toplevel = xdg_surface_get_toplevel(g.xdgSurface);
    xdg_toplevel_add_listener(g.toplevel, &kToplevel, nullptr);
    xdg_toplevel_set_title(g.toplevel, title);
    xdg_toplevel_set_app_id(g.toplevel, "vkworld");
    if (g.decorationManager) {
        g.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(g.decorationManager, g.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(g.decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(g.surface);
    for (int i = 0; i < 100 && !g.configured; ++i)   // (the first configure: before anything is drawn)
        if (wl_display_roundtrip(g.display) < 0) break;
    if (!g.configured) {
        std::fprintf(stderr, "Wayland: the window was never configured; using X11\n");
        disconnect();
        return false;
    }
    if (g.viewport && !g.fractionalScale && g.on.empty())   // (not on an output until it's drawn: start with theirs)
        for (const State::Output& out : g.outputs) g.scale = std::max(g.scale, (double)out.scale);
    std::fprintf(stderr, "Wayland: native window%s, scale %.2f\n", g.decoration ? " (server-side decorations)" : "", g.scale);
    return true;
}

void destroyWindow() {
    setCursorCaptured(false);
    disconnect();
}

void pollEvents() {
    if (!g.display) return;
    // Read what's arrived without waiting, then run the listeners
    while (wl_display_prepare_read(g.display) != 0) wl_display_dispatch_pending(g.display);
    wl_display_flush(g.display);
    pollfd fd{wl_display_get_fd(g.display), POLLIN, 0};
    if (poll(&fd, 1, 0) > 0) wl_display_read_events(g.display);
    else wl_display_cancel_read(g.display);
    wl_display_dispatch_pending(g.display);
    if (wl_display_get_error(g.display)) g.closeRequested = true;   // (the compositor's gone)
    repeatKeys();
    linux_shared::gamepadPoll();
}

bool waitEvents(double seconds) {
    if (!g.display) return false;
    while (wl_display_prepare_read(g.display) != 0) wl_display_dispatch_pending(g.display);
    wl_display_flush(g.display);
    pollfd fd{wl_display_get_fd(g.display), POLLIN, 0};
    bool got = poll(&fd, 1, (int)std::clamp(seconds * 1000.0, 0.0, 60000.0)) > 0;
    if (got) wl_display_read_events(g.display);   // (pollEvents runs the listeners)
    else wl_display_cancel_read(g.display);
    return got;
}

bool windowFocused() { return g.focused; }

bool closeRequested() { return g.closeRequested; }

void windowSize(uint32_t& width, uint32_t& height) {   // (physical pixels)
    width = g.width ? toPixels(g.width) : 0;
    height = g.height ? toPixels(g.height) : 0;
}

void setCursorCaptured(bool captured) {
    if (captured == g.captured || !g.display) return;
    g.mouseDx = g.mouseDy = 0.0;
    if (captured) {
        if (!g.pointer || !g.constraints || !g.relativeManager) return;   // (can't: stays free)
        g.captured = true;
        g.locked = zwp_pointer_constraints_v1_lock_pointer(g.constraints, g.surface, g.pointer, nullptr,
                                                           ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        g.relative = zwp_relative_pointer_manager_v1_get_relative_pointer(g.relativeManager, g.pointer);
        zwp_relative_pointer_v1_add_listener(g.relative, &kRelative, nullptr);
        showCursor(ImGuiMouseCursor_None);
    } else {
        g.captured = false;
        if (g.relative) zwp_relative_pointer_v1_destroy(g.relative);
        if (g.locked) zwp_locked_pointer_v1_destroy(g.locked);
        g.relative = nullptr;
        g.locked = nullptr;
        showCursor(g.imguiReady ? ImGui::GetMouseCursor() : ImGuiMouseCursor_Arrow);
    }
    wl_display_flush(g.display);
}

bool cursorCaptured() { return g.captured; }

void takeMouseDelta(float& dx, float& dy) {
    dx = (float)g.mouseDx;
    dy = (float)g.mouseDy;
    g.mouseDx = g.mouseDy = 0.0;
}

void setFullscreen(bool fullscreen) {
    if (!g.toplevel || fullscreen == g.fullscreen) return;
    if (fullscreen) xdg_toplevel_set_fullscreen(g.toplevel, nullptr);
    else xdg_toplevel_unset_fullscreen(g.toplevel);
    g.fullscreen = fullscreen;   // (the configure that follows confirms it)
    wl_display_flush(g.display);
}

bool isFullscreen() { return g.fullscreen; }

void setWindowIcon(const uint8_t*, uint32_t, uint32_t) {}   // (Wayland: the desktop entry's, by app id)

float dpiScale() { return (float)g.scale; }   // (the window is in physical pixels: the UI scales up with this)

const char* vulkanSurfaceExtension() { return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME; }

VkSurfaceKHR createVulkanSurface(VkInstance instance) {
    VkWaylandSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    ci.display = g.display;
    ci.surface = g.surface;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWaylandSurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS) return VK_NULL_HANDLE;
    return surface;
}

void imguiInit() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "vkworld_wayland";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    g.imguiReady = true;
    linux_shared::gamepadOpen();
}

void imguiNewFrame(float deltaTime) {
    ImGuiIO& io = ImGui::GetIO();
    uint32_t w = 0, h = 0;
    windowSize(w, h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = deltaTime > 0.f ? deltaTime : 1.f / 60.f;
    ImGuiMouseCursor want = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
    if (want != g.cursorShown && !g.captured) showCursor(want);
}

void imguiShutdown() {
    linux_shared::gamepadClose();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    g.imguiReady = false;
}

}  // namespace platform::wl
