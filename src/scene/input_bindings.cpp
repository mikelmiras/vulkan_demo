#include "scene/input_bindings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

std::vector<InputAction> defaultInputActions() {
    return {
        {"move_forward", {ImGuiKey_W, ImGuiKey_UpArrow, ImGuiKey_GamepadLStickUp}},
        {"move_back", {ImGuiKey_S, ImGuiKey_DownArrow, ImGuiKey_GamepadLStickDown}},
        {"move_left", {ImGuiKey_A, ImGuiKey_LeftArrow, ImGuiKey_GamepadLStickLeft}},
        {"move_right", {ImGuiKey_D, ImGuiKey_RightArrow, ImGuiKey_GamepadLStickRight}},
        {"move_up", {ImGuiKey_E, ImGuiKey_GamepadR1}},
        {"move_down", {ImGuiKey_Q, ImGuiKey_GamepadL1}},
        {"jump", {ImGuiKey_Space, ImGuiKey_GamepadFaceDown}},
        {"sprint", {ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_GamepadL3}},
        {"crouch", {ImGuiKey_C, ImGuiKey_LeftCtrl, ImGuiKey_GamepadFaceRight}},
        {"look", {}, ImGuiMouseButton_Right},   // hold to steer the view with the mouse (not needed while captured)
        {"look_left", {ImGuiKey_GamepadRStickLeft}},
        {"look_right", {ImGuiKey_GamepadRStickRight}},
        {"look_up", {ImGuiKey_GamepadRStickUp}},
        {"look_down", {ImGuiKey_GamepadRStickDown}},
        {"fire", {ImGuiKey_GamepadR2}, ImGuiMouseButton_Left},
        {"quicksave", {ImGuiKey_F5}},
        {"quickload", {ImGuiKey_F9}},
        {"pause", {ImGuiKey_P, ImGuiKey_GamepadStart}},
    };
}

std::vector<BindingConflict> bindingConflicts(const std::vector<InputAction>& actions) {
    std::vector<BindingConflict> out;
    for (size_t i = 0; i < actions.size(); ++i)
        for (size_t j = i + 1; j < actions.size(); ++j) {
            const InputAction &a = actions[i], &b = actions[j];
            for (ImGuiKey k : a.keys)
                if (std::find(b.keys.begin(), b.keys.end(), k) != b.keys.end()) out.push_back({a.name, b.name, keyName(k)});
            if (a.mouseButton >= 0 && a.mouseButton == b.mouseButton)
                out.push_back({a.name, b.name, "Mouse" + std::to_string(a.mouseButton)});
        }
    return out;
}

bool directionGroup(ImGuiKey key, ImGuiKey out[4]) {
    static const ImGuiKey kGroups[][4] = {
        {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight},
        {ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight},
        {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight},
        {ImGuiKey_W, ImGuiKey_S, ImGuiKey_A, ImGuiKey_D},
        {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow},
        {ImGuiKey_I, ImGuiKey_K, ImGuiKey_J, ImGuiKey_L},
        {ImGuiKey_Keypad8, ImGuiKey_Keypad5, ImGuiKey_Keypad4, ImGuiKey_Keypad6},
    };
    for (const auto& g : kGroups)
        if (std::find(g, g + 4, key) != g + 4) {
            std::copy(g, g + 4, out);
            return true;
        }
    return false;
}

const char* keyName(ImGuiKey key) { return ImGui::GetKeyName(key); }   // a static table: no context needed

ImGuiKey keyFromName(const std::string& name) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (name == ImGui::GetKeyName((ImGuiKey)k)) return (ImGuiKey)k;
    return ImGuiKey_None;
}

bool isGamepadKey(ImGuiKey key) { return key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown; }

std::string bindingString(const InputAction& action) {
    std::string out;
    for (ImGuiKey k : action.keys) {
        if (!out.empty()) out += ",";
        out += keyName(k);
    }
    if (action.mouseButton >= 0) out += (out.empty() ? "Mouse" : ",Mouse") + std::to_string(action.mouseButton);
    return out;
}

bool parseBinding(const std::string& text, InputAction& action) {
    std::vector<ImGuiKey> keys;
    int mouse = -1;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = std::min(text.find(',', at), text.size());
        std::string word = text.substr(at, end - at);
        word.erase(0, word.find_first_not_of(" \t"));
        word.erase(word.find_last_not_of(" \t") + 1);
        if (word.size() == 6 && word.rfind("Mouse", 0) == 0 && std::isdigit((unsigned char)word[5])) mouse = word[5] - '0';
        else if (ImGuiKey k = keyFromName(word); k != ImGuiKey_None) keys.push_back(k);
        at = end + 1;
    }
    if (keys.empty() && mouse < 0) return false;
    action.keys = keys;
    action.mouseButton = mouse;
    return true;
}

PadType padTypeOf(uint16_t vendor, const std::string& name) {
    if (vendor == 0x054c) return PadType::PlayStation;   // Sony
    if (vendor == 0x057e) return PadType::Switch;        // Nintendo
    if (vendor == 0x045e) return PadType::Xbox;          // Microsoft
    std::string n = name;
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (has("dualsense") || has("dualshock") || has("playstation") || has("ps4") || has("ps5") || has("sony") ||
        has("wireless controller"))   // (what Sony's pads call themselves)
        return PadType::PlayStation;
    if (has("switch") || has("nintendo") || has("joy-con") || has("pro controller")) return PadType::Switch;
    if (has("xbox") || has("x-box") || has("xinput") || has("microsoft")) return PadType::Xbox;
    return PadType::Generic;
}

const char* padTypeName(PadType type) {
    switch (type) {
    case PadType::Xbox: return "xbox";
    case PadType::PlayStation: return "playstation";
    case PadType::Switch: return "switch";
    default: return "generic";
    }
}

ImGuiKey linuxPadButton(uint16_t code, PadType type) {
    bool positional = type == PadType::PlayStation || type == PadType::Switch;   // (BTN_NORTH / BTN_WEST mean that)
    switch (code) {
    case 0x130: return ImGuiKey_GamepadFaceDown;    // BTN_SOUTH (BTN_A)
    case 0x131: return ImGuiKey_GamepadFaceRight;   // BTN_EAST (BTN_B)
    case 0x133: return positional ? ImGuiKey_GamepadFaceUp : ImGuiKey_GamepadFaceLeft;    // BTN_NORTH / BTN_X
    case 0x134: return positional ? ImGuiKey_GamepadFaceLeft : ImGuiKey_GamepadFaceUp;    // BTN_WEST / BTN_Y
    case 0x136: return ImGuiKey_GamepadL1;          // BTN_TL
    case 0x137: return ImGuiKey_GamepadR1;          // BTN_TR
    case 0x138: return ImGuiKey_GamepadL2;          // BTN_TL2 (digital triggers)
    case 0x139: return ImGuiKey_GamepadR2;          // BTN_TR2
    case 0x13a: return ImGuiKey_GamepadBack;        // BTN_SELECT
    case 0x13b: return ImGuiKey_GamepadStart;       // BTN_START
    case 0x13d: return ImGuiKey_GamepadL3;          // BTN_THUMBL
    case 0x13e: return ImGuiKey_GamepadR3;          // BTN_THUMBR
    case 0x220: return ImGuiKey_GamepadDpadUp;      // BTN_DPAD_UP (pads that have the d-pad as buttons)
    case 0x221: return ImGuiKey_GamepadDpadDown;
    case 0x222: return ImGuiKey_GamepadDpadLeft;
    case 0x223: return ImGuiKey_GamepadDpadRight;
    default: return ImGuiKey_None;                  // (BTN_MODE, the guide button: the system's)
    }
}

bool linuxPadAxis(uint8_t code, ImGuiKey& negative, ImGuiKey& positive, ImGuiKey& trigger) {
    negative = positive = trigger = ImGuiKey_None;
    switch (code) {
    case 0x00: negative = ImGuiKey_GamepadLStickLeft, positive = ImGuiKey_GamepadLStickRight; return true;   // ABS_X
    case 0x01: negative = ImGuiKey_GamepadLStickUp, positive = ImGuiKey_GamepadLStickDown; return true;      // ABS_Y
    case 0x03: negative = ImGuiKey_GamepadRStickLeft, positive = ImGuiKey_GamepadRStickRight; return true;   // ABS_RX
    case 0x04: negative = ImGuiKey_GamepadRStickUp, positive = ImGuiKey_GamepadRStickDown; return true;      // ABS_RY
    case 0x10: negative = ImGuiKey_GamepadDpadLeft, positive = ImGuiKey_GamepadDpadRight; return true;       // ABS_HAT0X
    case 0x11: negative = ImGuiKey_GamepadDpadUp, positive = ImGuiKey_GamepadDpadDown; return true;          // ABS_HAT0Y
    case 0x02: trigger = ImGuiKey_GamepadL2; return true;                                                    // ABS_Z
    case 0x05: trigger = ImGuiKey_GamepadR2; return true;                                                    // ABS_RZ
    case 0x0a: trigger = ImGuiKey_GamepadL2; return true;                                                    // ABS_BRAKE
    case 0x09: trigger = ImGuiKey_GamepadR2; return true;                                                    // ABS_GAS
    default: return false;
    }
}
