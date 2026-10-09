// Named input actions for game code ("move_forward", "jump", ...), so gameplay never looks at raw keys.
// Reads key/mouse state through Dear ImGui, which every platform layer already feeds.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

class InputMap {
public:
    struct Action {
        std::string name;
        std::vector<ImGuiKey> keys;      // any of these keys triggers the action
        int mouseButton = -1;            // ImGuiMouseButton, or -1
    };

    InputMap();   // default bindings, see input.cpp

    // Once per rendered frame. Disabled devices read as released (e.g. keys while typing in a text field, the mouse
    // while it's over an editor panel). Edges and mouse movement are latched until the next fixed update consumes them, so a press is never
    // lost or seen twice whatever the frame rate.
    void beginFrame(bool keyboard, bool mouse);
    void endFixedStep();   // after each fixed update

    bool down(const std::string& action) const;
    bool pressed(const std::string& action) const;    // went down since the previous fixed update
    bool released(const std::string& action) const;
    float axis(const std::string& negative, const std::string& positive) const;   // -1, 0 or 1
    ImVec2 mouseDelta() const { return mouseDelta_; }   // pixels since the previous fixed update

    const std::vector<Action>& actions() const { return actions_; }

private:
    int find(const std::string& name) const;

    std::vector<Action> actions_;
    std::vector<bool> down_, pressed_, released_;
    ImVec2 mouseDelta_{0, 0};
};
