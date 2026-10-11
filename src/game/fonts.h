// Fonts for the game's UI (GameUI::text): Roboto (compiled in) unless the game asks for a font file (TTF / OTF,
// relative to the project). Dear ImGui draws them at any size, any glyph of the font made the first time it's used,
// so UTF-8 text in any script the font has just works.
#pragma once

#include <string>

struct ImFont;

namespace gamefonts {

// The font for `file` ("" = the default). A file not loaded yet is loaded before the next frame (fonts can't be
// added mid-frame); until then (or if it can't be read) the default is used.
ImFont* font(const std::string& file);
// Loads what font() was asked for: the host calls it before ImGui::NewFrame()
void loadPending();

}  // namespace gamefonts
