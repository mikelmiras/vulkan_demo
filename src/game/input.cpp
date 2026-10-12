#include "game/input.h"

#include <algorithm>

InputMap::InputMap(std::vector<InputAction> actions) : actions_(std::move(actions)) {
    value_.assign(actions_.size(), 0.f);
    down_.assign(actions_.size(), false);
    pressed_.assign(actions_.size(), false);
    released_.assign(actions_.size(), false);
}

void InputMap::beginFrame(bool keyboard, bool mouse, ImVec2 mouseDelta) {
    ImGuiIO& io = ImGui::GetIO();
    framePressed_.assign(actions_.size(), false);
    for (size_t i = 0; i < actions_.size(); ++i) {
        const InputAction& a = actions_[i];
        float v = 0.f;
        if (keyboard)
            for (ImGuiKey k : a.keys) {
                if (isGamepadKey(k)) v = std::max(v, io.KeysData[k - ImGuiKey_NamedKey_BEGIN].AnalogValue);
                else if (ImGui::IsKeyDown(k)) v = 1.f;
            }
        if (mouse && a.mouseButton >= 0 && ImGui::IsMouseDown(a.mouseButton)) v = 1.f;
        bool now = v > 0.5f;
        if (now && !down_[i]) pressed_[i] = framePressed_[i] = true;
        if (!now && down_[i]) released_[i] = true;
        down_[i] = now;
        value_[i] = v;
    }
    if (mouse) {
        mouseDelta_.x += mouseDelta.x;
        mouseDelta_.y += mouseDelta.y;
    }
}

void InputMap::applyRemote(const std::vector<float>& values, ImVec2 mouseDelta) {
    for (size_t i = 0; i < actions_.size(); ++i) {
        float v = i < values.size() ? values[i] : 0.f;
        bool now = v > 0.5f;
        if (now && !down_[i]) pressed_[i] = true;
        if (!now && down_[i]) released_[i] = true;
        down_[i] = now;
        value_[i] = v;
    }
    mouseDelta_.x += mouseDelta.x;
    mouseDelta_.y += mouseDelta.y;
}

void InputMap::endFixedStep() {
    pressed_.assign(actions_.size(), false);
    released_.assign(actions_.size(), false);
    mouseDelta_ = {0, 0};
}

bool InputMap::pressedThisFrame(const std::string& action) const {
    int i = find(action);
    return i >= 0 && (size_t)i < framePressed_.size() && framePressed_[(size_t)i];
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

float InputMap::value(const std::string& action) const {
    int i = find(action);
    return i >= 0 ? value_[(size_t)i] : 0.f;
}

float InputMap::axis(const std::string& negative, const std::string& positive) const {
    return value(positive) - value(negative);
}
