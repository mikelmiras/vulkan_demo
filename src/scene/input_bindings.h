// Input action bindings: which keys, mouse button and gamepad controls trigger each named action.
// Saved with the scene (so the standalone game gets the same controls) and edited in Game > Input bindings.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

struct InputAction {
    std::string name;
    std::vector<ImGuiKey> keys;   // keyboard and gamepad (ImGuiKey_Gamepad*, analog for sticks and triggers)
    int mouseButton = -1;         // ImGuiMouseButton, or -1
};

std::vector<InputAction> defaultInputActions();

// Stable key names for files ("W", "Space", "GamepadFaceDown", ...); ImGuiKey_None if unknown
const char* keyName(ImGuiKey key);
ImGuiKey keyFromName(const std::string& name);
bool isGamepadKey(ImGuiKey key);
