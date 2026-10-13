// A game's own interface (HUD, menus), drawn by behaviours: Behaviour::drawUI() gets a GameUI every rendered frame,
// also while the game is paused.
//
// Positions are on a canvas 1080 units high and width() wide (following the window's aspect ratio), so a layout
// looks the same at any resolution; (0, 0) is the top-left corner of the game view. Everything is drawn over the
// 3D view and under the editor's panels. Buttons are hit-tested here (not ImGui widgets), so the UI never takes
// input away from the game or the editor.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

struct Color {
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
};

class GameUI {
public:
    enum class Align { Left, Center, Right };

    float width() const { return width_; }
    float height() const { return 1080.f; }

    void rect(float x, float y, float w, float h, Color color, float rounding = 0.f);
    void border(float x, float y, float w, float h, Color color, float thickness = 3.f, float rounding = 0.f);
    // `y` is the top of the text; with Align::Center, `x` is its middle (Right: its right end). A soft shadow keeps
    // it readable over the scene.
    // UTF-8, in the game's font (Roboto) or `font`: a TTF / OTF file in the project (game/fonts.h)
    void text(float x, float y, const std::string& text, float size = 36.f, Color color = {}, Align align = Align::Left,
              const std::string& font = "");
    float textWidth(const std::string& text, float size = 36.f, const std::string& font = "") const;
    // Text wrapped to `w` (lines break where UAX #14 allows: game/line_break.h; a word too long on its own is cut),
    // lines 1.2 sizes apart, each aligned within [x, x + w]; returns its height. textBoxHeight measures without drawing.
    float textBox(float x, float y, float w, const std::string& text, float size = 36.f, Color color = {},
                  Align align = Align::Left, const std::string& font = "");
    float textBoxHeight(float w, const std::string& text, float size = 36.f, const std::string& font = "") const;
    // Where a caret can go in a line of text: (byte offset, x from the text's left), one per character boundary in
    // the text's order (right-to-left text runs right to left: game/text_shaping.h's caretStops)
    std::vector<std::pair<size_t, float>> caretStops(const std::string& text, float size = 36.f, const std::string& font = "") const;
    // A bar filled to `fraction` (0..1) from the left
    void bar(float x, float y, float w, float h, float fraction, Color fill, Color back = {0.f, 0.f, 0.f, 0.5f});
    // A texture by name (a built-in like "Checker", or an image file, relative to the project)
    void image(const std::string& texture, float x, float y, float w, float h, Color tint = {});
    // A clickable box with a centred label; true on the frame it's clicked. Clicks only count while the mouse
    // pointer is visible: call showCursor() in the frames a menu is up.
    bool button(float x, float y, float w, float h, const std::string& label, float textSize = 36.f);
    bool hovered(float x, float y, float w, float h) const;
    // The mouse on the canvas (when the pointer is free and over the game), and its left button
    bool mouseAvailable() const { return mouse_; }
    ImVec2 mouse() const;
    bool mouseDown() const;
    bool mouseClicked() const;
    float scale() const { return scale_; }   // window pixels per canvas unit
    ImVec2 origin() const { return {x0_, y0_}; }   // the canvas's top left in the window
    void showCursor() { wantsCursor_ = true; }   // this frame needs the mouse pointer: the host frees the cursor

    // ---- Host side (the editor's play mode, vkgame)
    using TextureLookup = std::function<ImTextureID(const std::string& name)>;   // 0: not available (yet)
    void setTextureLookup(TextureLookup lookup) { textures_ = std::move(lookup); }
    // Starts a frame drawing into `dl`, over the game view at (x, y) of size (w, h) window pixels. `mouse`: the
    // pointer is free and over the game view (not captured, not over an editor panel)
    void begin(ImDrawList* dl, float x, float y, float w, float h, bool mouse);
    void end();
    bool wantsCursor() const { return wantsCursor_; }

private:
    ImVec2 toScreen(float x, float y) const { return {x0_ + x * scale_, y0_ + y * scale_}; }
    ImU32 rgba(Color c) const;

    ImDrawList* dl_ = nullptr;
    float x0_ = 0.f, y0_ = 0.f, scale_ = 1.f, width_ = 1920.f;
    bool mouse_ = false, wantsCursor_ = false;
    TextureLookup textures_;
};
