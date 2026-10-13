// Text shaping: turning a line of UTF-8 into positioned glyphs, so scripts whose letters change shape and join
// (Arabic, the Indic scripts), stack marks (Thai, Vietnamese) or run right to left (Hebrew, Arabic) come out right.
//
//   1. Bidi: each character's embedding level (the Unicode Bidirectional Algorithm, UAX #9, without explicit
//      embeddings and isolates: the paragraph's direction from its first strong character; numbers, neutrals and
//      marks resolved between them), then runs of one level put in visual order.
//   2. Itemising: runs split where the level, the script or the font changes (the first font in the chain that has
//      the character: the text's font, then fallbacks such as CJK or emoji fonts).
//   3. Shaping each run with HarfBuzz (ligatures, contextual forms, mark positioning, kerning, mirrored brackets in
//      right-to-left runs), its neighbours as context, and the runs' glyphs laid out left to right.
//
// Plain code (no ImGui): GameUI::text draws the glyphs through game/fonts.h, tests check the layout.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shaping {

std::vector<uint32_t> decodeUtf8(const std::string& text);   // (bad bytes: U+FFFD)

// ---- Bidi
enum class BidiClass : uint8_t { L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON };
BidiClass bidiClass(uint32_t cp);
int paragraphLevel(const std::vector<uint32_t>& text);   // 0: left to right, 1: right to left
std::vector<uint8_t> bidiLevels(const std::vector<uint32_t>& text, int paragraph);
struct Run {
    size_t start = 0, end = 0;   // characters [start, end)
    uint8_t level = 0;           // odd: right to left
};
// Runs of one level, in visual order (left to right on screen)
std::vector<Run> visualRuns(const std::vector<uint8_t>& levels);

// Whether a line needs shaping (or a fallback font) at all: anything from Hebrew on, or combining marks
bool needsShaping(const std::string& utf8);

// ---- Fonts and shaping
struct Glyph {
    int face = 0;          // which face of the chain it's from
    uint32_t glyph = 0;    // its index in that font (0: none had the character)
    float x = 0.f, y = 0.f;   // where its origin goes, in ems from the line's start (y down)
    uint32_t cluster = 0;  // the character it came from
};
struct Shaped {
    std::vector<Glyph> glyphs;
    float width = 0.f;     // in ems
    std::vector<uint32_t> missing;   // characters no face of the chain had
};

class Shaper {
public:
    Shaper();
    ~Shaper();
    Shaper(const Shaper&) = delete;
    Shaper& operator=(const Shaper&) = delete;

    // A font file's bytes (TTF / OTF / a TTC's `index`): a face to shape with; -1 if it can't be read
    int addFace(std::shared_ptr<const std::string> data, int index = 0);
    int faceCount() const;
    bool hasGlyph(int face, uint32_t cp) const;
    float ascent(int face) const;   // in ems (its hhea / OS/2 ascender)
    // One line (no line breaks) with the faces in `chain` (the first that has a character draws it)
    Shaped shape(const std::string& utf8, const std::vector<int>& chain) const;

private:
    struct Face;
    std::vector<std::unique_ptr<Face>> faces_;
};

}  // namespace shaping
