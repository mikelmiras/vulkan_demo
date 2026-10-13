// Button prompts: what to press for an action, shown the way the player's device labels it (a keycap for keys,
// the pad's own buttons: Xbox letters, PlayStation shapes, Switch letters in their places).
//
// A prompt is text with glyphs in it: GameUI::text draws them (so they work in any text, in any font). A pad button
// is one private-use character (U+E000 + pad type * 32 + the button's place from ImGuiKey_GamepadStart), a key is
// its label between U+E100 and U+E101. Texts get them from "{button:jump}" (the game's formatting: the device used
// last), ctx.prompt("jump"), or Lua's input.prompt("jump").
#pragma once

#include <string>
#include <vector>

#include "imgui.h"
#include "scene/input_bindings.h"

namespace prompts {

enum class Device { Keyboard, Pad };

std::string padGlyph(PadType type, ImGuiKey key);   // (a gamepad key; "" for others)
std::string keyCap(const std::string& label);
std::string keyLabel(ImGuiKey key);                  // "Space", "W", "Left Shift", "Esc", "Up"
std::string mouseLabel(int button);                  // "Left click", "Right click", "Middle click", "Mouse 4"
std::string padLabel(PadType type, ImGuiKey key);    // in words: "A", "Cross", "B" (Switch's bottom), "LB", "L1", "ZL"
// The action's first binding on that device, as glyph text ("" if it has none there)
std::string forAction(const InputAction& action, Device device, PadType type);
// ... and in words ("Space", "Cross", "Left click"): for the plain text of menus and logs
std::string wordsForAction(const InputAction& action, Device device, PadType type);

struct Piece {
    enum Kind { Text, Pad, Key } kind = Text;
    std::string text;            // (Text: the run; Key: its label)
    PadType type = PadType::Generic;
    ImGuiKey key = ImGuiKey_None;
};
bool hasGlyphs(const std::string& text);
std::vector<Piece> split(const std::string& text);

// A glyph's width at text size `size` (pixels), and drawing it with its top left at `pos`
float glyphWidth(const Piece& piece, float size, ImFont* font);
void drawGlyph(ImDrawList* dl, ImFont* font, ImVec2 pos, float size, const Piece& piece, float alpha);

}  // namespace prompts
