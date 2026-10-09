#include "game/input.h"

InputMap::InputMap() {
    actions_ = {
        {"move_forward", {ImGuiKey_W, ImGuiKey_UpArrow}},
        {"move_back", {ImGuiKey_S, ImGuiKey_DownArrow}},
        {"move_left", {ImGuiKey_A, ImGuiKey_LeftArrow}},
        {"move_right", {ImGuiKey_D, ImGuiKey_RightArrow}},
        {"move_up", {ImGuiKey_E}},
        {"move_down", {ImGuiKey_Q}},
        {"jump", {ImGuiKey_Space}},
        {"sprint", {ImGuiKey_LeftShift, ImGuiKey_RightShift}},
        {"look", {}, ImGuiMouseButton_Right},   // hold to steer the view with the mouse
        {"fire", {}, ImGuiMouseButton_Left},
    };
    down_.assign(actions_.size(), false);
    pressed_.assign(actions_.size(), false);
    released_.assign(actions_.size(), false);
}

void InputMap::beginFrame(bool keyboard, bool mouse) {
    for (size_t i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i];
        bool now = false;
        if (keyboard)
            for (ImGuiKey k : a.keys) now = now || ImGui::IsKeyDown(k);
        if (mouse && a.mouseButton >= 0) now = now || ImGui::IsMouseDown(a.mouseButton);
        if (now && !down_[i]) pressed_[i] = true;
        if (!now && down_[i]) released_[i] = true;
        down_[i] = now;
    }
    if (mouse) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        mouseDelta_.x += d.x;
        mouseDelta_.y += d.y;
    }
}

void InputMap::endFixedStep() {
    pressed_.assign(actions_.size(), false);
    released_.assign(actions_.size(), false);
    mouseDelta_ = {0, 0};
}

int InputMap::find(const std::string& name) const {
    for (size_t i = 0; i < actions_.size(); ++i)
        if (actions_[i].name == name) return (int)i;
    return -1;
}

bool InputMap::down(const std::string& action) const {
    int i = find(action);
    return i >= 0 && down_[(size_t)i];
}

bool InputMap::pressed(const std::string& action) const {
    int i = find(action);
    return i >= 0 && pressed_[(size_t)i];
}

bool InputMap::released(const std::string& action) const {
    int i = find(action);
    return i >= 0 && released_[(size_t)i];
}

float InputMap::axis(const std::string& negative, const std::string& positive) const {
    return (down(positive) ? 1.f : 0.f) - (down(negative) ? 1.f : 0.f);
}
