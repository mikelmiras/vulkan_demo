// Line breaking (UAX #14, the Unicode Line Breaking Algorithm) and wrapping text to a width: where a line may end
// (after spaces, after hyphens, between CJK characters, never before closing punctuation or inside a number like
// "-3.5" or "$20", nor inside an emoji sequence), then lines filled greedily. Character classes come from ranges
// (the common scripts, CJK, Hangul, emoji) and the general category; Thai, Lao, Khmer and Myanmar (which need a
// dictionary) break only at spaces. Plain code: GameUI's text boxes wrap with it, tests check it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace linebreak {

enum class Class : uint8_t {
    BK, CR, LF, NL, SP, ZW, GL, WJ, CM, ZWJ, OP, CL, CP, QU, IS, NU, PR, PO, SY, HY, BA, BB, B2, EX, IN, NS, ID, AL,
    RI, EB, EM, H2, H3, JL, JV, JT
};
Class classOf(uint32_t cp);

enum class Break : uint8_t { None, Allowed, Mandatory };
// For each character, whether a line may end before it (the first: None)
std::vector<Break> opportunities(const std::vector<uint32_t>& text);

// A line of wrapped text: bytes [start, end) of the UTF-8, its trailing spaces and the line break left out
struct Line {
    size_t start = 0, end = 0;
};
// Lines no wider than `maxWidth` as `measure` (a piece of the text -> its width) says; a word wider than that on
// its own is broken between characters
std::vector<Line> wrap(const std::string& utf8, float maxWidth, const std::function<float(const std::string&)>& measure);

}  // namespace linebreak
