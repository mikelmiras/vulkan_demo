// Fonts for shaped text (game/text_shaping.h): Dear ImGui finds glyphs by character, shaping gives glyph indices, so
// shaped text is drawn with an ImFont whose "characters" are its font's glyph indices (plus one: 0 isn't a
// character), baked by this loader (stb_truetype by glyph index). All of them share one baseline (setBaseline: the
// game font's), so glyphs from a fallback font sit on the same line as the main one's.
#pragma once

#include <cstdint>

#include "imgui.h"

struct ImFontLoader;

namespace glyphfont {

const ImFontLoader* loader();
inline ImWchar charFor(uint32_t glyph) { return (ImWchar)(glyph + 1); }   // (glyph indices up to 65534)
// The baseline below the top of a line, as a fraction of the text size (every glyph font's)
void setBaseline(float fraction);
float baseline();
// A font file's ascent as a fraction of its em (hhea), or 0.8 if it can't be read
float ascentOf(const void* data, int index = 0);

}  // namespace glyphfont
