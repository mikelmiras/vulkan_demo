// Game UI layouts: the UIElement components of a scene (scene/scene.h) placed, drawn and driven. The Game runs them
// every rendered frame after the behaviours' own drawUI; the editor draws them over its viewport to lay them out.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/ui.h"
#include "scene/scene.h"

class GameState;

struct UIRect {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;   // canvas units (1080 high), top left
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct UILayout {
    std::unordered_map<EntityId, UIRect> rects;
    std::vector<EntityId> order;   // drawing order: parents before their children, in the scene's order
};

// Every visible UI element's rectangle on a canvas `width` units wide (GameUI::width())
UILayout layoutUI(const Scene& scene, float width);
bool focusable(const UIElement& u);   // buttons, sliders, toggles, text fields

// Something a UI element asked for (a button pressed): its action, run by the Game
struct UIAction {
    EntityId element = kNoEntity;
    std::string action, target;
    float value = 0.f;     // a slider's value
    std::string text;      // a text field's
};

// Focus and interaction, kept between frames
class UIController {
public:
    struct Frame {
        GameState* state = nullptr;   // values sliders, toggles and text fields show and change (null: none)
        std::function<std::string(const std::string&)> format;   // text -> what's shown (null: as it is)
        bool keyboard = true;         // reads keys and the gamepad (not while an editor text box has them)
        bool interactive = true;      // false: just drawn (the editor's preview)
    };
    // Draws the scene's UI and handles the mouse, keys and gamepad; pressed buttons' actions go to `actions`
    void frame(GameUI& ui, Scene& scene, const UILayout& layout, const Frame& f, std::vector<UIAction>& actions);

    EntityId focused() const { return focused_; }
    void setFocus(EntityId id) { focused_ = id; }
    bool editingText() const { return editing_ != kNoEntity; }   // a text field takes the keyboard
    bool hasMenu() const { return hasMenu_; }   // something could be pressed last frame (the pointer is freed)

private:
    EntityId focused_ = kNoEntity, editing_ = kNoEntity, dragging_ = kNoEntity;
    bool hasMenu_ = false;
};
