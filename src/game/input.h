// Named input actions for game code ("move_forward", "jump", ...), so gameplay never looks at raw keys.
// Reads keyboard, mouse and gamepad state through Dear ImGui, which every platform layer already feeds.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"
#include "scene/input_bindings.h"

class InputMap {
public:
    explicit InputMap(std::vector<InputAction> actions = defaultInputActions());

    // Once per rendered frame. Disabled devices read as released (e.g. keys while typing in a text field, the mouse
    // while it's over an editor panel); the gamepad counts as part of the keyboard. `mouseDelta` is the mouse
    // movement this frame (raw while the cursor is captured). Edges and mouse movement are latched until the next
    // fixed update consumes them, so a press is never lost or seen twice whatever the frame rate.
    void beginFrame(bool keyboard, bool mouse, ImVec2 mouseDelta);
    void endFixedStep();   // after each fixed update

    bool down(const std::string& action) const;
    bool pressed(const std::string& action) const;    // went down since the previous fixed update
    bool released(const std::string& action) const;
    float value(const std::string& action) const;     // 0..1: analog for sticks and triggers, else 0 or 1
    float axis(const std::string& negative, const std::string& positive) const;   // -1..1
    ImVec2 mouseDelta() const { return mouseDelta_; }  // pixels since the previous fixed update

    // While the cursor is captured (hidden, locked to the window) mouse movement is always for the game
    void setMouseCaptured(bool captured) { mouseCaptured_ = captured; }
    bool mouseCaptured() const { return mouseCaptured_; }

    const std::vector<InputAction>& actions() const { return actions_; }

private:
    int find(const std::string& name) const;

    std::vector<InputAction> actions_;
    std::vector<float> value_;
    std::vector<bool> down_, pressed_, released_;
    ImVec2 mouseDelta_{0, 0};
    bool mouseCaptured_ = false;
};
