// Input action bindings: which keys, mouse button and gamepad controls trigger each named action.
// Saved with the scene (so the standalone game gets the same controls) and edited in Game > Input bindings.
#pragma once

#include <cstdint>
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

// A player's own bindings for an action, as saved in their settings: key names and "Mouse<n>", comma-separated
// ("Space,GamepadFaceDown,Mouse0"); "" when parsing finds nothing
std::string bindingString(const InputAction& action);
bool parseBinding(const std::string& text, InputAction& action);   // (replaces its keys and mouse button)

// Actions sharing a control (a key, a pad control, a mouse button): each pair once, with that control's name
struct BindingConflict {
    std::string first, second, control;
};
std::vector<BindingConflict> bindingConflicts(const std::vector<InputAction>& actions);

// Sticks as a whole: the group a control belongs to (a stick, the d-pad, or a cluster of keys: WASD, the arrows,
// IJKL, the number pad's 8 4 5 6) as its four directions, up, down, left, right; false if it's in none
bool directionGroup(ImGuiKey key, ImGuiKey out[4]);

// ---- Gamepads, one by one (platform::gamepads(): what each pad has down; Dear ImGui sees them all together)
enum class PadType { Generic, Xbox, PlayStation, Switch };
constexpr int kGamepadKeys = ImGuiKey_GamepadRStickDown - ImGuiKey_GamepadStart + 1;
struct GamepadState {
    bool connected = false;
    PadType type = PadType::Generic;
    std::string name;
    float values[kGamepadKeys] = {};   // ImGui's gamepad keys from ImGuiKey_GamepadStart, 0..1 (analog: sticks, triggers)
    float value(ImGuiKey key) const { return isGamepadKey(key) ? values[key - ImGuiKey_GamepadStart] : 0.f; }
    void set(ImGuiKey key, float v) { if (isGamepadKey(key)) values[key - ImGuiKey_GamepadStart] = v; }
};
// Which kind of pad it is, from its USB vendor id (0 if unknown) and name
PadType padTypeOf(uint16_t vendor, const std::string& name);
const char* padTypeName(PadType type);   // "xbox", "playstation", "switch", "generic"

// Linux: where a joystick-API button or axis goes, by its evdev code (JSIOCGBTNMAP / JSIOCGAXMAP give each index's).
// Face buttons are by position (ImGui's FaceDown is the bottom one); xpad's BTN_X / BTN_Y codes are the left and top
// buttons, where Sony's and Nintendo's drivers use the same codes for top and left (BTN_NORTH / BTN_WEST).
ImGuiKey linuxPadButton(uint16_t code, PadType type);
// An axis: its negative and positive gamepad keys (sticks, the d-pad hat), or a trigger (`trigger` set)
bool linuxPadAxis(uint8_t code, ImGuiKey& negative, ImGuiKey& positive, ImGuiKey& trigger);
