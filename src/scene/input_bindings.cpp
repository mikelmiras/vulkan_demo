#include "scene/input_bindings.h"

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

const char* keyName(ImGuiKey key) { return ImGui::GetKeyName(key); }   // a static table: no context needed

ImGuiKey keyFromName(const std::string& name) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (name == ImGui::GetKeyName((ImGuiKey)k)) return (ImGuiKey)k;
    return ImGuiKey_None;
}

bool isGamepadKey(ImGuiKey key) { return key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown; }
