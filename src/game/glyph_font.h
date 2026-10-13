// Fonts for shaped text (game/text_shaping.h): Dear ImGui finds glyphs by character, shaping gives glyph indices, so
// shaped text is drawn with an ImFont whose "characters" are its font's glyph indices (moved up past the characters
// Dear ImGui handles itself: charFor), baked by this loader (stb_truetype by glyph index; colour glyphs from
// game/color_glyphs). All of them share one baseline (setBaseline: the game font's), so glyphs from a fallback font
// sit on the same line as the main one's.
#pragma once

#include <cstdint>

#include "imgui.h"

struct ImFontLoader;

namespace glyphfont {

const ImFontLoader* loader();
// (From U+0100 up: Dear ImGui treats some characters below that specially, the space's glyph always hidden; glyph
// indices up to 65279)
constexpr uint32_t kFirstChar = 0x100;
inline ImWchar charFor(uint32_t glyph) { return (ImWchar)(glyph + kFirstChar); }
// The baseline below the top of a line, as a fraction of the text size (every glyph font's)
void setBaseline(float fraction);
float baseline();
// A font file's ascent as a fraction of its em (hhea), or 0.8 if it can't be read
float ascentOf(const void* data, int index = 0);
// Dear ImGui sizes a font by its ascent-to-descent height, shaping by its em: a text size in ems is this much of
// the size ImGui draws at (the game font's em over its height; setEmScale: what fonts.cpp found for it)
float emScaleOf(const void* data, int index = 0);
void setEmScale(float scale);
float emScale();

}  // namespace glyphfont
