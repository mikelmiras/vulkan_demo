// game/line_break: UAX #14 break opportunities (spaces, hyphens, CJK, punctuation, numbers, emoji sequences) and
// wrapping to a width; text_shaping's caret positions for text fields
#include "test.h"

#include "game/line_break.h"
#include "game/text_shaping.h"

using linebreak::Break;

namespace {

// Where breaks may go, as character indices (mandatory ones marked with a minus)
std::vector<int> breaksIn(const std::string& utf8) {
    std::vector<Break> b = linebreak::opportunities(shaping::decodeUtf8(utf8));
    std::vector<int> out;
    for (size_t i = 0; i < b.size(); ++i)
        if (b[i] != Break::None) out.push_back(b[i] == Break::Mandatory ? -(int)i : (int)i);
    return out;
}

// Wrapped lines, measuring one unit a character
std::vector<std::string> lines(const std::string& text, float width) {
    std::vector<std::string> out;
    auto measure = [](const std::string& s) { return (float)shaping::decodeUtf8(s).size(); };
    for (const linebreak::Line& l : linebreak::wrap(text, width, measure)) out.push_back(text.substr(l.start, l.end - l.start));
    return out;
}

}  // namespace

TEST(line_break_classes) {
    using C = linebreak::Class;
    CHECK(linebreak::classOf(' ') == C::SP);
    CHECK(linebreak::classOf('a') == C::AL);
    CHECK(linebreak::classOf('7') == C::NU);
    CHECK(linebreak::classOf('(') == C::OP && linebreak::classOf(')') == C::CP);
    CHECK(linebreak::classOf('-') == C::HY);
    CHECK(linebreak::classOf(0x4E00) == C::ID);    // 一
    CHECK(linebreak::classOf(0x3002) == C::CL);    // 。
    CHECK(linebreak::classOf(0x00A0) == C::GL);    // no-break space
    CHECK(linebreak::classOf(0x0301) == C::CM);    // a combining accent
    CHECK(linebreak::classOf(0x1F1EF) == C::RI);
    CHECK(linebreak::classOf(0xAC00) == C::H2 && linebreak::classOf(0xAC01) == C::H3);
}

TEST(line_break_opportunities) {
    CHECK((breaksIn("Hello world") == std::vector<int>{6}));          // after the space
    CHECK((breaksIn("well-known") == std::vector<int>{5}));           // after the hyphen
    CHECK(breaksIn("-3.5").empty());                                   // (a number)
    CHECK(breaksIn("$20").empty());
    CHECK(breaksIn("50%").empty());
    CHECK((breaksIn("(word) next") == std::vector<int>{7}));          // not inside the brackets' edges
    CHECK(breaksIn("don't").empty());
    CHECK(breaksIn("a\xC2\xA0" "b").empty());                         // no-break space
    CHECK((breaksIn("ab\ncd") == std::vector<int>{-3}));               // a line break: mandatory
    CHECK((breaksIn("ab\r\ncd") == std::vector<int>{-4}));             // (CR LF together)
    CHECK((breaksIn("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") == std::vector<int>{1, 2}));   // 日本語: between any two
    CHECK((breaksIn("\xE6\x97\xA5\xE6\x9C\xAC\xE3\x80\x82") == std::vector<int>{1}));       // 日本。: not before 。
    CHECK(breaksIn("e\xCC\x81" "e").empty());                          // (a mark stays with its letter)
    // Emoji: a family (joined with ZWJ) stays together; two flags break between them, not inside one
    CHECK(breaksIn("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7").empty());
    CHECK((breaksIn("\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7") == std::vector<int>{2}));
    CHECK(breaksIn("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD").empty());      // 👍🏽 (with its skin tone)
    CHECK(breaksIn("").empty());
}

TEST(line_wrapping) {
    CHECK((lines("the quick brown fox", 10.f) == std::vector<std::string>{"the quick", "brown fox"}));
    CHECK((lines("the quick brown fox", 100.f) == std::vector<std::string>{"the quick brown fox"}));
    CHECK((lines("abcdefghijklm", 5.f) == std::vector<std::string>{"abcde", "fghij", "klm"}));   // (too long: cut)
    CHECK((lines("ab\n  cd", 50.f) == std::vector<std::string>{"ab", "  cd"}));   // (an indent after a line break stays)
    CHECK((lines("ab\n\ncd", 50.f) == std::vector<std::string>{"ab", "", "cd"}));
    CHECK((lines("go   on", 3.f) == std::vector<std::string>{"go", "on"}));       // (the spaces go with the first line)
    CHECK((lines("", 5.f) == std::vector<std::string>{""}));
    // CJK wraps anywhere; an accent isn't cut from its letter
    CHECK((lines("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 2.f) == std::vector<std::string>{"\xE6\x97\xA5\xE6\x9C\xAC", "\xE8\xAA\x9E"}));
    CHECK((lines("e\xCC\x81" "e\xCC\x81" "e\xCC\x81", 2.f) == std::vector<std::string>{"e\xCC\x81", "e\xCC\x81", "e\xCC\x81"}));
}

// A caret's places in a line: left to right text left to right; in a right-to-left run, from its right end leftwards
TEST(caret_stops) {
    const uint32_t ALEF = 0x05D0, BET = 0x05D1;
    std::vector<uint32_t> text = {'a', 'b', ALEF, BET};   // shown "a b BET ALEF"
    shaping::Shaped sh;
    auto glyph = [](uint32_t cluster, float start) {
        shaping::Glyph g;
        g.cluster = cluster;
        g.start = start;
        g.advance = 1.f;
        return g;
    };
    sh.glyphs = {glyph(0, 0.f), glyph(1, 1.f), glyph(3, 2.f), glyph(2, 3.f)};
    sh.width = 4.f;
    std::vector<float> stops = shaping::caretStops(text, sh);
    REQUIRE(stops.size() == 5);
    CHECK(stops[0] == 0.f && stops[1] == 1.f);   // before a, before b
    CHECK(stops[2] == 4.f);                      // before alef: its right edge (the run's right end)
    CHECK(stops[3] == 3.f);                      // before bet
    CHECK(stops[4] == 2.f);                      // the end: bet's left edge
    // A ligature: one glyph for two characters, each half of it
    shaping::Shaped lig;
    shaping::Glyph g = glyph(0, 0.f);
    g.advance = 2.f;
    lig.glyphs = {g};
    stops = shaping::caretStops({'f', 'i'}, lig);
    CHECK(stops[0] == 0.f && stops[1] == 1.f && stops[2] == 2.f);
}
