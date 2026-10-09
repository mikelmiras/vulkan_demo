#include "platform/imgui_impl_xcb.h"

#include <cfloat>
#include <cstdlib>
#include <vector>

#include "imgui.h"

namespace {

struct BackendData {
    xcb_connection_t* conn = nullptr;
    xcb_window_t window = 0;
    xcb_keycode_t minKeycode = 0;
    int keysymsPerKeycode = 0;
    std::vector<uint32_t> keysyms;
    bool ctrl[2] = {}, shift[2] = {}, alt[2] = {}, super[2] = {};
};

BackendData* g_bd = nullptr;

// X modifier masks
constexpr uint16_t MASK_SHIFT = XCB_MOD_MASK_SHIFT, MASK_LOCK = XCB_MOD_MASK_LOCK,
                   MASK_CTRL = XCB_MOD_MASK_CONTROL, MASK_ALT = XCB_MOD_MASK_1, MASK_NUMLOCK = XCB_MOD_MASK_2,
                   MASK_ALTGR = XCB_MOD_MASK_5;

uint32_t keysymAt(xcb_keycode_t code, int column) {
    BackendData* bd = g_bd;
    if (code < bd->minKeycode || column >= bd->keysymsPerKeycode) return 0;
    size_t i = (size_t)(code - bd->minKeycode) * bd->keysymsPerKeycode + column;
    return i < bd->keysyms.size() ? bd->keysyms[i] : 0;
}

void loadKeymap() {
    BackendData* bd = g_bd;
    const xcb_setup_t* setup = xcb_get_setup(bd->conn);
    bd->minKeycode = setup->min_keycode;
    auto cookie = xcb_get_keyboard_mapping(bd->conn, setup->min_keycode,
                                           (uint8_t)(setup->max_keycode - setup->min_keycode + 1));
    xcb_get_keyboard_mapping_reply_t* reply = xcb_get_keyboard_mapping_reply(bd->conn, cookie, nullptr);
    if (!reply) return;
    bd->keysymsPerKeycode = reply->keysyms_per_keycode;
    xcb_keysym_t* syms = xcb_get_keyboard_mapping_keysyms(reply);
    int n = xcb_get_keyboard_mapping_keysyms_length(reply);
    bd->keysyms.assign(syms, syms + n);
    free(reply);
}

ImGuiKey keysymToImGuiKey(uint32_t ks) {
    if (ks >= 'a' && ks <= 'z') return (ImGuiKey)(ImGuiKey_A + (ks - 'a'));
    if (ks >= 'A' && ks <= 'Z') return (ImGuiKey)(ImGuiKey_A + (ks - 'A'));
    if (ks >= '0' && ks <= '9') return (ImGuiKey)(ImGuiKey_0 + (ks - '0'));
    if (ks >= 0xffbe && ks <= 0xffc9) return (ImGuiKey)(ImGuiKey_F1 + (ks - 0xffbe));
    if (ks >= 0xffb0 && ks <= 0xffb9) return (ImGuiKey)(ImGuiKey_Keypad0 + (ks - 0xffb0));
    switch (ks) {
        case 0xff09: return ImGuiKey_Tab;
        case 0xff51: case 0xff96: return ImGuiKey_LeftArrow;
        case 0xff52: case 0xff97: return ImGuiKey_UpArrow;
        case 0xff53: case 0xff98: return ImGuiKey_RightArrow;
        case 0xff54: case 0xff99: return ImGuiKey_DownArrow;
        case 0xff55: case 0xff9a: return ImGuiKey_PageUp;
        case 0xff56: case 0xff9b: return ImGuiKey_PageDown;
        case 0xff50: case 0xff95: return ImGuiKey_Home;
        case 0xff57: case 0xff9c: return ImGuiKey_End;
        case 0xff63: case 0xff9e: return ImGuiKey_Insert;
        case 0xffff: case 0xff9f: return ImGuiKey_Delete;
        case 0xff08: return ImGuiKey_Backspace;
        case 0x0020: return ImGuiKey_Space;
        case 0xff0d: return ImGuiKey_Enter;
        case 0xff8d: return ImGuiKey_KeypadEnter;
        case 0xff1b: return ImGuiKey_Escape;
        case 0xffe1: return ImGuiKey_LeftShift;
        case 0xffe2: return ImGuiKey_RightShift;
        case 0xffe3: return ImGuiKey_LeftCtrl;
        case 0xffe4: return ImGuiKey_RightCtrl;
        case 0xffe9: case 0xffe7: return ImGuiKey_LeftAlt;
        case 0xffea: case 0xfe03: return ImGuiKey_RightAlt;
        case 0xffeb: return ImGuiKey_LeftSuper;
        case 0xffec: return ImGuiKey_RightSuper;
        case 0xff67: return ImGuiKey_Menu;
        case 0xffab: return ImGuiKey_KeypadAdd;
        case 0xffad: return ImGuiKey_KeypadSubtract;
        case 0xffaa: return ImGuiKey_KeypadMultiply;
        case 0xffaf: return ImGuiKey_KeypadDivide;
        case 0xffae: return ImGuiKey_KeypadDecimal;
        case '-': return ImGuiKey_Minus;
        case '=': return ImGuiKey_Equal;
        case ',': return ImGuiKey_Comma;
        case '.': return ImGuiKey_Period;
        case '/': return ImGuiKey_Slash;
        case ';': return ImGuiKey_Semicolon;
        case '\'': return ImGuiKey_Apostrophe;
        case '[': return ImGuiKey_LeftBracket;
        case ']': return ImGuiKey_RightBracket;
        case '\\': return ImGuiKey_Backslash;
        case '`': return ImGuiKey_GraveAccent;
        default: return ImGuiKey_None;
    }
}

void updateModifiers(ImGuiKey key, bool down) {
    BackendData* bd = g_bd;
    ImGuiIO& io = ImGui::GetIO();
    switch (key) {
        case ImGuiKey_LeftCtrl: bd->ctrl[0] = down; break;
        case ImGuiKey_RightCtrl: bd->ctrl[1] = down; break;
        case ImGuiKey_LeftShift: bd->shift[0] = down; break;
        case ImGuiKey_RightShift: bd->shift[1] = down; break;
        case ImGuiKey_LeftAlt: bd->alt[0] = down; break;
        case ImGuiKey_RightAlt: bd->alt[1] = down; break;
        case ImGuiKey_LeftSuper: bd->super[0] = down; break;
        case ImGuiKey_RightSuper: bd->super[1] = down; break;
        default: return;
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, bd->ctrl[0] || bd->ctrl[1]);
    io.AddKeyEvent(ImGuiMod_Shift, bd->shift[0] || bd->shift[1]);
    io.AddKeyEvent(ImGuiMod_Alt, bd->alt[0] || bd->alt[1]);
    io.AddKeyEvent(ImGuiMod_Super, bd->super[0] || bd->super[1]);
}

void addTextInput(const xcb_key_press_event_t* e) {
    if (e->state & (MASK_CTRL | MASK_ALT)) return;
    bool shift = (e->state & MASK_SHIFT) != 0;
    uint32_t base = keysymAt(e->detail, 0);

    int column = shift ? 1 : 0;
    if (base >= 0xff80 && base <= 0xffbd)                  // keypad: NumLock selects the digit column
        column = ((e->state & MASK_NUMLOCK) != 0) != shift ? 1 : 0;
    if (e->state & MASK_ALTGR) column += 4;                // ISO level 3 (AltGr) lives in columns 4/5

    uint32_t ks = keysymAt(e->detail, column);
    if (!ks) ks = keysymAt(e->detail, column & 1);
    if (!ks) ks = base;
    if ((e->state & MASK_LOCK) && ks >= 'a' && ks <= 'z') ks -= 32;
    else if ((e->state & MASK_LOCK) && ks >= 'A' && ks <= 'Z') ks += 32;

    uint32_t cp = 0;
    if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff)) cp = ks;   // Latin-1 == Unicode
    else if (ks >= 0x01000000 && ks <= 0x0110ffff) cp = ks - 0x01000000;    // direct Unicode keysyms
    else if (ks >= 0xffb0 && ks <= 0xffb9) cp = '0' + (ks - 0xffb0);
    else if (ks == 0xffae) cp = '.';
    else if (ks == 0xffad) cp = '-';
    else if (ks == 0xffab) cp = '+';
    else if (ks == 0xffaa) cp = '*';
    else if (ks == 0xffaf) cp = '/';
    if (cp) ImGui::GetIO().AddInputCharacter(cp);
}

void processKey(const xcb_key_press_event_t* e, bool down, bool repeat) {
    ImGuiKey key = keysymToImGuiKey(keysymAt(e->detail, 0));
    if (!repeat && key != ImGuiKey_None) {
        ImGui::GetIO().AddKeyEvent(key, down);
        updateModifiers(key, down);
    }
    if (down) addTextInput(e);
}

// Returns true if the event was consumed as input
bool processInputEvent(xcb_generic_event_t* e) {
    ImGuiIO& io = ImGui::GetIO();
    switch (e->response_type & 0x7f) {
        case XCB_KEY_PRESS:
            processKey((xcb_key_press_event_t*)e, true, false);
            return true;
        case XCB_KEY_RELEASE:
            processKey((xcb_key_press_event_t*)e, false, false);
            return true;
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE: {
            auto* b = (xcb_button_press_event_t*)e;
            bool down = (e->response_type & 0x7f) == XCB_BUTTON_PRESS;
            io.AddMousePosEvent((float)b->event_x, (float)b->event_y);
            switch (b->detail) {
                case 1: io.AddMouseButtonEvent(0, down); break;
                case 3: io.AddMouseButtonEvent(1, down); break;
                case 2: io.AddMouseButtonEvent(2, down); break;
                case 8: io.AddMouseButtonEvent(3, down); break;
                case 9: io.AddMouseButtonEvent(4, down); break;
                case 4: if (down) io.AddMouseWheelEvent(0.f, 1.f); break;
                case 5: if (down) io.AddMouseWheelEvent(0.f, -1.f); break;
                case 6: if (down) io.AddMouseWheelEvent(1.f, 0.f); break;
                case 7: if (down) io.AddMouseWheelEvent(-1.f, 0.f); break;
            }
            return true;
        }
        case XCB_MOTION_NOTIFY: {
            auto* m = (xcb_motion_notify_event_t*)e;
            io.AddMousePosEvent((float)m->event_x, (float)m->event_y);
            return true;
        }
        case XCB_LEAVE_NOTIFY: {
            auto* l = (xcb_leave_notify_event_t*)e;
            if (!(l->state & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3)))
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            return true;
        }
        case XCB_FOCUS_IN:
            io.AddFocusEvent(true);
            return true;
        case XCB_FOCUS_OUT: {
            io.AddFocusEvent(false);   // ImGui releases all keys
            BackendData* bd = g_bd;
            bd->ctrl[0] = bd->ctrl[1] = bd->shift[0] = bd->shift[1] = false;
            bd->alt[0] = bd->alt[1] = bd->super[0] = bd->super[1] = false;
            return true;
        }
        case XCB_MAPPING_NOTIFY:
            loadKeymap();
            return true;
    }
    return false;
}

}  // namespace

bool ImGui_ImplXcb_Init(xcb_connection_t* connection, xcb_window_t window) {
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");
    g_bd = new BackendData();
    g_bd->conn = connection;
    g_bd->window = window;
    io.BackendPlatformUserData = g_bd;
    io.BackendPlatformName = "imgui_impl_xcb";
    loadKeymap();
    return true;
}

void ImGui_ImplXcb_Shutdown() {
    ImGuiIO& io = ImGui::GetIO();
    delete g_bd;
    g_bd = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.BackendPlatformName = nullptr;
}

void ImGui_ImplXcb_NewFrame(float displayWidth, float displayHeight, float deltaTime) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(displayWidth, displayHeight);
    io.DeltaTime = deltaTime > 0.f ? deltaTime : 1.f / 60.f;
}

void ImGui_ImplXcb_PollEvents(void (*handler)(void* user, xcb_generic_event_t* event), void* user) {
    xcb_connection_t* conn = g_bd->conn;
    xcb_generic_event_t* pending = nullptr;
    for (;;) {
        xcb_generic_event_t* e = pending ? pending : xcb_poll_for_event(conn);
        pending = nullptr;
        if (!e) break;

        // X auto-repeat sends KeyRelease immediately followed by KeyPress with the same timestamp.
        if ((e->response_type & 0x7f) == XCB_KEY_RELEASE) {
            xcb_generic_event_t* next = xcb_poll_for_event(conn);
            if (next && (next->response_type & 0x7f) == XCB_KEY_PRESS) {
                auto* rel = (xcb_key_release_event_t*)e;
                auto* press = (xcb_key_press_event_t*)next;
                if (press->detail == rel->detail && press->time == rel->time) {
                    processKey(press, true, true);   // repeat: text input only, key stays down
                    free(e);
                    free(next);
                    continue;
                }
            }
            pending = next;
        }

        if (!processInputEvent(e) && handler) handler(user, e);
        free(e);
    }
}
