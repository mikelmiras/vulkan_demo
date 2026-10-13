// Text shaping (game/text_shaping.h): UTF-8, bidi levels and visual order, which text needs shaping, HarfBuzz
// shaping of Arabic (joining forms, the lam-alef ligature, marks, right to left), fallback fonts through
// game/fonts.h, and GameUI drawing shaped text
#include "test.h"

#include <fstream>
#include <iterator>
#include <memory>

#include "game/fonts.h"
#include "game/glyph_font.h"
#include "game/text_shaping.h"
#include "game/ui.h"
#include "imgui.h"

namespace {

const char* kArabicFont = VKWORLD_TEST_ASSETS "/fonts/NotoNaskhArabic-Regular.ttf";   // (SIL Open Font License)

std::shared_ptr<const std::string> fileBytes(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::make_shared<std::string>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The text's characters in visual order (its runs reversed where they're right to left)
std::vector<uint32_t> visual(const std::vector<uint32_t>& text) {
    std::vector<uint8_t> levels = shaping::bidiLevels(text, shaping::paragraphLevel(text));
    std::vector<uint32_t> out;
    for (const shaping::Run& r : shaping::visualRuns(levels)) {
        if (r.level & 1)
            for (size_t i = r.end; i-- > r.start;) out.push_back(text[i]);
        else
            for (size_t i = r.start; i < r.end; ++i) out.push_back(text[i]);
    }
    return out;
}

}  // namespace

TEST(utf8_decoding) {
    std::vector<uint32_t> cps = shaping::decodeUtf8("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");   // a é € 😀
    REQUIRE(cps.size() == 4);
    CHECK(cps[0] == 'a' && cps[1] == 0xE9 && cps[2] == 0x20AC && cps[3] == 0x1F600);
    std::vector<uint32_t> bad = shaping::decodeUtf8("x\xC3(\xE2\x82");   // (a cut sequence: replacement characters)
    REQUIRE(bad.size() >= 3);
    CHECK(bad[0] == 'x' && bad[1] == 0xFFFD && bad[2] == '(');
    CHECK(shaping::decodeUtf8("").empty());
}

TEST(bidi_levels_and_order) {
    using shaping::BidiClass;
    CHECK(shaping::bidiClass('a') == BidiClass::L);
    CHECK(shaping::bidiClass(0x05D0) == BidiClass::R);    // alef (Hebrew)
    CHECK(shaping::bidiClass(0x0627) == BidiClass::AL);   // alef (Arabic)
    CHECK(shaping::bidiClass('7') == BidiClass::EN);
    CHECK(shaping::bidiClass(0x0663) == BidiClass::AN);
    CHECK(shaping::bidiClass(' ') == BidiClass::WS);
    CHECK(shaping::bidiClass(0x064E) == BidiClass::NSM);  // fatha
    CHECK(shaping::bidiClass('!') == BidiClass::ON);
    // "abc DEF ghi" with DEF Hebrew: one right-to-left run in the middle
    std::vector<uint32_t> t = {'a', 'b', ' ', 0x05D0, 0x05D1, 0x05D2, ' ', 'c'};
    CHECK(shaping::paragraphLevel(t) == 0);
    std::vector<uint8_t> lv = shaping::bidiLevels(t, 0);
    CHECK(lv[0] == 0 && lv[2] == 0 && lv[3] == 1 && lv[5] == 1 && lv[6] == 0 && lv[7] == 0);
    std::vector<uint32_t> v = visual(t);
    CHECK(v[3] == 0x05D2 && v[5] == 0x05D0);   // (shown reversed)
    // A Hebrew paragraph with a number: the paragraph runs right to left, the digits stay in their order
    std::vector<uint32_t> h = {0x05E9, 0x05DC, ' ', '1', '2', '3', ' ', 0x05E2};
    CHECK(shaping::paragraphLevel(h) == 1);
    lv = shaping::bidiLevels(h, 1);
    CHECK(lv[0] == 1 && lv[2] == 1 && lv[3] == 2 && lv[5] == 2);
    v = visual(h);
    REQUIRE(v.size() == h.size());
    CHECK(v[0] == 0x05E2);                                // (the last word first, on the left)
    CHECK(v[2] == '1' && v[3] == '2' && v[4] == '3');     // (the number reads left to right)
    CHECK(v[7] == 0x05E9);
    // Numbers after Arabic letters are Arabic numbers; a separator between digits joins them
    std::vector<uint32_t> a = {0x0639, ' ', '1', '.', '5'};
    lv = shaping::bidiLevels(a, shaping::paragraphLevel(a));
    CHECK(lv[2] == 2 && lv[3] == 2 && lv[4] == 2);
    // Marks go with their letter; trailing spaces at the paragraph's level
    std::vector<uint32_t> m = {'a', ' ', 0x0628, 0x064E, ' ', ' '};
    lv = shaping::bidiLevels(m, 0);
    CHECK(lv[2] == 1 && lv[3] == 1 && lv[4] == 0 && lv[5] == 0);
    // Runs in visual order
    std::vector<shaping::Run> runs = shaping::visualRuns({0, 0, 1, 1, 0});
    REQUIRE(runs.size() == 3);
    CHECK(runs[1].start == 2 && runs[1].end == 4 && runs[1].level == 1);
    runs = shaping::visualRuns({1, 1, 2, 2, 1});   // (inside right to left: the left-to-right part stays put)
    REQUIRE(runs.size() == 3);
    CHECK(runs[0].start == 4 && runs[1].start == 2 && runs[2].start == 0);
}

TEST(text_needing_shaping) {
    CHECK(!shaping::needsShaping("Hello, world!"));
    CHECK(!shaping::needsShaping("Caf\xC3\xA9 na\xC3\xAFve"));          // (Latin with accents)
    CHECK(!shaping::needsShaping("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));   // Привет
    CHECK(!shaping::needsShaping("\xE2\x80\x94 \xE2\x82\xAC 5"));       // (dashes, the euro)
    CHECK(shaping::needsShaping("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"));    // שלום
    CHECK(shaping::needsShaping("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"));    // سلام
    CHECK(shaping::needsShaping("e\xCC\x81"));                           // e + a combining acute
    CHECK(shaping::needsShaping("\xE6\x97\xA5\xE6\x9C\xAC"));            // 日本
    CHECK(shaping::needsShaping("\xF0\x9F\x98\x80"));                    // 😀
}

// Arabic with HarfBuzz: letters join (their forms in a word differ from the letters alone), the word runs right to
// left (its first letter drawn last, on the right), marks sit on their letters
TEST(arabic_shaping) {
    shaping::Shaper shaper;
    auto font = fileBytes(kArabicFont);
    REQUIRE(font->size() > 1000);
    int face = shaper.addFace(font);
    REQUIRE(face == 0);
    CHECK(shaper.addFace(std::make_shared<std::string>("not a font")) == -1);
    CHECK(shaper.hasGlyph(face, 0x0633));
    CHECK(!shaper.hasGlyph(face, 'Q'));
    CHECK(shaper.ascent(face) > 0.5f && shaper.ascent(face) < 2.5f);
    const std::string salam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";   // سلام: seen, lam, alef, meem
    shaping::Shaped s = shaper.shape(salam, {face});
    CHECK(s.missing.empty());
    REQUIRE(s.glyphs.size() == 4);   // (this font joins lam and alef as two glyphs)
    CHECK(s.glyphs.back().cluster == 0 && s.glyphs.front().cluster == 3);   // (visual order: meem left, seen right)
    for (size_t i = 1; i < s.glyphs.size(); ++i) CHECK(s.glyphs[i - 1].x < s.glyphs[i].x);
    CHECK(s.width > 0.5f);
    auto alone = [&](const char* letter) {
        shaping::Shaped one = shaper.shape(letter, {face});
        return one.glyphs.size() == 1 ? one.glyphs[0].glyph : 0u;
    };
    CHECK(alone("\xD8\xB3") != s.glyphs.back().glyph);   // seen: its initial form in the word
    CHECK(alone("\xD9\x85") == s.glyphs.front().glyph);  // meem: alone after all (alef doesn't join what follows)
    CHECK(alone("\xD9\x84") != s.glyphs[2].glyph);       // lam: joined on both sides (with alef's form)
    shaping::Shaped marked = shaper.shape("\xD8\xA8\xD9\x8E", {face});   // beh + fatha
    REQUIRE(marked.glyphs.size() >= 2);   // (the letter, its fatha; this font draws dots as glyphs of their own too)
    CHECK_NEAR(marked.width, shaper.shape("\xD8\xA8", {face}).width, 1e-4);   // (the mark on the letter: no room of its own)
    // What the font lacks is reported (a fallback's job)
    shaping::Shaped latin = shaper.shape("ab " + salam, {face});
    CHECK(latin.missing.size() == 2);
    // A chain: the first face that has a character draws it (here the same font twice: the first)
    int again = shaper.addFace(font);
    shaping::Shaped chained = shaper.shape(salam, {again, face});
    for (const shaping::Glyph& g : chained.glyphs) CHECK(g.face == again);
}

// Through game/fonts.h: Roboto for Latin, the Arabic font as a fallback once loaded (before the next frame), glyph
// fonts made for both, and GameUI drawing and measuring the shaped line
TEST(shaped_text_with_fallbacks) {
    test::ImGuiContextGuard imgui;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // (glyphs baked as they're needed, as the renderer allows)
    const std::string text = "Hi \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";   // Hi سلام
    gamefonts::addFallback(kArabicFont);
    const shaping::Shaped* first = gamefonts::shaped(text, "");
    REQUIRE(first);
    CHECK(!first->missing.empty());   // (not loaded yet: boxes for a frame)
    gamefonts::loadPending();
    const shaping::Shaped* s = gamefonts::shaped(text, "");
    REQUIRE(s);
    CHECK(s->missing.empty());
    REQUIRE(s->glyphs.size() == 7);   // H, i, space, then the word's four
    int latin = s->glyphs[0].face, arabic = s->glyphs.back().face;
    CHECK(latin != arabic);
    CHECK(s->glyphs[1].face == latin);
    for (size_t i = 3; i < s->glyphs.size(); ++i) CHECK(s->glyphs[i].face == arabic);
    CHECK(s->glyphs[0].x < s->glyphs[3].x);   // (Latin on the left: a left-to-right paragraph)
    CHECK(gamefonts::glyphFont(latin) && gamefonts::glyphFont(arabic));
    CHECK(glyphfont::baseline() > 0.7f && glyphfont::baseline() < 1.1f);   // (Roboto's ascent)
    GameUI ui;
    ImGui::NewFrame();
    ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, 1920.f, 1080.f, false);
    CHECK_NEAR(ui.textWidth(text, 40.f), s->width * 40.f, 0.5);
    ui.text(100.f, 100.f, text, 40.f);
    ui.text(100.f, 200.f, text + "\nsecond line", 40.f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    CHECK(dl->VtxBuffer.Size > 6 * 4);   // (quads drawn for the glyphs)
    ui.end();
    ImGui::EndFrame();
}
