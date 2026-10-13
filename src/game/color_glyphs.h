// Colour glyphs (emoji) read straight from a font's tables, the three ways fonts store them:
//   CBDT / CBLC   PNG bitmaps per glyph in strikes of one pixel size (Noto Color Emoji: Linux, Android)
//   sbix          the same idea, Apple's table (Apple Color Emoji)
//   COLR / CPAL   version 0: a glyph drawn as layers of other (outline) glyphs, each in a palette colour
//                 (Segoe UI Emoji on Windows)
// Plain functions over the font file's bytes: game/glyph_font.cpp rasterises with them, tests check them on tables
// made by hand. COLR version 1 (gradients, transforms) isn't read: such glyphs fall back to their outlines.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace colorglyphs {

// A table of the font at `fontOffset` (a TTC's face): where it starts in `data` and its length; false if it has none
bool findTable(const uint8_t* data, size_t size, uint32_t fontOffset, const char tag[4], uint32_t& offset, uint32_t& length);

// A glyph's bitmap from the strike whose size is nearest above `pixels` (else the biggest)
struct Bitmap {
    const uint8_t* png = nullptr;   // the PNG's bytes, inside the font's data
    uint32_t pngSize = 0;
    float ppem = 0.f;               // the strike's size: pixels per em
    float bearingX = 0.f;           // the image's left edge from the glyph's origin, in the strike's pixels
    float bearingY = 0.f;           // ... and its top above the baseline
    float width = 0.f, height = 0.f;   // (0: take the PNG's)
    bool bottomOrigin = false;      // (sbix) bearingY is the image's bottom edge, not its top
};
bool findBitmap(const uint8_t* data, size_t size, uint32_t fontOffset, uint32_t glyph, float pixels, Bitmap& out);

// A COLR version 0 glyph's layers, bottom first: glyphs to draw in their colours
struct Layer {
    uint32_t glyph = 0;
    uint8_t rgba[4] = {255, 255, 255, 255};
    bool foreground = false;   // palette entry 0xFFFF: the text's own colour
};
bool colrLayers(const uint8_t* data, size_t size, uint32_t fontOffset, uint32_t glyph, std::vector<Layer>& out);

// Does the font have any colour glyph tables (CBDT, sbix or COLR)?
bool hasColor(const uint8_t* data, size_t size, uint32_t fontOffset);

// For fonts with no outlines (bitmap-only emoji fonts, which stb_truetype won't open): their glyph count (maxp),
// units per em (head) and a glyph's advance in those units (hhea / hmtx)
int glyphCount(const uint8_t* data, size_t size, uint32_t fontOffset);
int unitsPerEm(const uint8_t* data, size_t size, uint32_t fontOffset);
int advance(const uint8_t* data, size_t size, uint32_t fontOffset, uint32_t glyph);

}  // namespace colorglyphs
