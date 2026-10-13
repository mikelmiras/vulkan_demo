// Fonts for the game's UI (GameUI::text): Roboto (compiled in) unless the game asks for a font file (TTF / OTF,
// relative to the project). Dear ImGui draws them at any size, any glyph of the font made the first time it's used.
//
// Text that needs shaping (game/text_shaping.h: right-to-left and joining scripts, combining marks, CJK, emoji) is
// laid out with HarfBuzz instead, with fallback fonts for what the font lacks: the project's own (fonts/fallback/*.ttf
// / .otf / .ttc, in name order), then the system's for that script (fontconfig on Linux, the usual fonts of Windows
// and macOS), loaded the first time a script turns up. Its glyphs are drawn through fonts by glyph index
// (game/glyph_font.h).
#pragma once

#include <filesystem>
#include <string>

#include "game/text_shaping.h"

struct ImFont;

namespace gamefonts {

// The font for `file` ("" = the default). A file not loaded yet is loaded before the next frame (fonts can't be
// added mid-frame); until then (or if it can't be read) the default is used.
ImFont* font(const std::string& file);
// Loads what font() and shaped() were asked for: the host calls it before ImGui::NewFrame()
void loadPending();

// A line of text shaped with `file`'s font and the fallbacks (cached); null without an ImGui context. Fallbacks it
// lacks are found before the next frame (until then their characters are boxes).
const shaping::Shaped* shaped(const std::string& text, const std::string& file);
// The font drawing a face's glyphs (shaped(): Glyph::face), or null until it's made (before the next frame)
ImFont* glyphFont(int face);
// One more fallback, tried before the system's (tests; a game's code)
void addFallback(const std::filesystem::path& file, int index = 0);

}  // namespace gamefonts
