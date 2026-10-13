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

// Explicit embeddings, overrides and isolates (UAX #9 X1-X10)
TEST(bidi_explicit_controls) {
    const uint32_t LRE = 0x202A, RLE = 0x202B, PDF = 0x202C, RLO = 0x202E, LRI = 0x2066, RLI = 0x2067, FSI = 0x2068,
                   PDI = 0x2069, ALEF = 0x05D0, BET = 0x05D1;
    CHECK(shaping::bidiClass(RLO) == shaping::BidiClass::RLO);
    CHECK(shaping::bidiClass(FSI) == shaping::BidiClass::FSI);
    // An override: left-to-right letters shown right to left
    std::vector<uint32_t> o = {'a', RLO, 'b', 'c', PDF, 'd'};
    std::vector<uint8_t> lv = shaping::bidiLevels(o, 0);
    CHECK(lv[0] == 0 && lv[2] == 1 && lv[3] == 1 && lv[5] == 0);
    std::vector<uint32_t> v = visual(o);
    std::vector<uint32_t> letters;
    for (uint32_t c : v)
        if (c >= 'a' && c <= 'z') letters.push_back(c);
    CHECK((letters == std::vector<uint32_t>{'a', 'c', 'b', 'd'}));
    // An embedding: right to left around left-to-right letters (level 1 + 1)
    lv = shaping::bidiLevels({'a', RLE, 'b', PDF, 'c'}, 0);
    CHECK(lv[0] == 0 && lv[2] == 2 && lv[4] == 0);
    lv = shaping::bidiLevels({ALEF, LRE, BET, PDF}, 1);   // (left to right inside right to left: 2, its R 3)
    CHECK(lv[0] == 1 && lv[2] == 3);
    // An isolate keeps what's around it from what's inside: "a [Hebrew] 1" has the number left to right after it,
    // where without the isolate it would follow the Hebrew
    std::vector<uint32_t> iso = {'a', ' ', FSI, ALEF, BET, PDI, ' ', '1'};
    lv = shaping::bidiLevels(iso, shaping::paragraphLevel(iso));
    CHECK(lv[3] == 1 && lv[4] == 1);   // (FSI: right to left, from its first strong character)
    CHECK(lv[2] == 0 && lv[5] == 0);   // (the isolate's ends: outside it)
    CHECK(lv[7] == 0);
    std::vector<uint32_t> plain = {'a', ' ', ALEF, BET, ' ', '1'};
    CHECK(shaping::bidiLevels(plain, 0)[5] == 2);
    lv = shaping::bidiLevels({'a', LRI, ALEF, PDI}, 0);   // (left-to-right isolate: level 2, Hebrew in it 3)
    CHECK(lv[2] == 3);
    lv = shaping::bidiLevels({'a', RLI, 'b', PDI}, 0);
    CHECK(lv[2] == 2);
    // The paragraph's direction skips what's in isolates
    CHECK(shaping::paragraphLevel({RLI, ALEF, PDI, 'a'}) == 0);
    CHECK(shaping::paragraphLevel({LRI, 'a', PDI, ALEF}) == 1);
    // An unmatched PDF / PDI does nothing; too deep a nesting stops at 125 levels without failing
    lv = shaping::bidiLevels({'a', PDF, PDI, 'b'}, 0);
    CHECK(lv[0] == 0 && lv[3] == 0);
    std::vector<uint32_t> deep(200, RLE);
    deep.push_back('x');
    lv = shaping::bidiLevels(deep, 0);
    CHECK(lv.back() >= 124 && lv.back() <= 126);
    // Text with the controls goes through shaping (for the bidi pass)
    CHECK(shaping::needsShaping("a\xE2\x80\xAE" "bc"));   // RLO
    CHECK(shaping::needsShaping("a\xE2\x81\xA8" "b"));    // FSI
    CHECK(!shaping::needsShaping("a\xE2\x80\x94" "b"));   // (an em dash: still as it is)
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
    // A context whose renderer takes new glyphs as they're needed (as the real one does): no atlas built up front
    struct Context {
        Context() {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.DisplaySize = ImVec2(1920, 1080);
            io.IniFilename = nullptr;
        }
        ~Context() { ImGui::DestroyContext(); }
    } context;
    const std::string text = "Hi \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";   // Hi سلام
    gamefonts::addFallback(kArabicFont);
    const shaping::Shaped* first = gamefonts::shaped(text, "");
    REQUIRE(first);
    CHECK(!first->missing.empty());   // (not loaded yet: boxes for a frame)
    gamefonts::font("");               // (Roboto itself too, to compare with)
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
    // Glyph 31 (Roboto's semicolon) isn't drawn as character 32, the space Dear ImGui always hides
    CHECK(glyphfont::charFor(0) >= 0x100 && glyphfont::charFor(31) != ' ');
    GameUI ui;
    ImGui::NewFrame();
    ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, 1920.f, 1080.f, false);
    CHECK(glyphfont::emScale() > 0.7f && glyphfont::emScale() < 1.f);   // (Roboto: its em is 0.85 of its height)
    CHECK_NEAR(ui.textWidth(text, 40.f), s->width * 40.f * glyphfont::emScale(), 0.5);
    // A Latin word as tall shaped (with an emoji or Arabic beside it) as on its own
    const shaping::Shaped* hi = gamefonts::shaped("Hi", "");
    REQUIRE(hi);
    CHECK_NEAR(hi->width * 40.f * glyphfont::emScale(), gamefonts::font("")->CalcTextSizeA(40.f, FLT_MAX, 0.f, "Hi").x, 1.0);
    ui.text(100.f, 100.f, text, 40.f);
    ui.text(100.f, 200.f, text + "\nsecond line", 40.f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    CHECK(dl->VtxBuffer.Size > 6 * 4);   // (quads drawn for the glyphs)
    ui.end();
    ImGui::EndFrame();
}

// GameUI's text boxes: wrapped to their width with the font's real widths
TEST(game_ui_text_box) {
    struct Context {
        Context() {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.DisplaySize = ImVec2(1920, 1080);
            io.IniFilename = nullptr;
        }
        ~Context() { ImGui::DestroyContext(); }
    } context;
    gamefonts::loadPending();
    GameUI ui;
    ImGui::NewFrame();
    ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, 1920.f, 1080.f, false);
    const std::string text = "The quick brown fox jumps over the lazy dog";
    float one = ui.textWidth(text, 40.f);
    CHECK(ui.textBoxHeight(one + 10.f, text, 40.f) == 48.f);   // (fits: one line, 1.2 sizes)
    float h = ui.textBoxHeight(one * 0.4f, text, 40.f);
    CHECK(h >= 48.f * 3.f && h <= 48.f * 4.f);                 // (about two fifths wide: three lines or so)
    CHECK(ui.textBox(100.f, 100.f, one * 0.4f, text, 40.f) == h);
    CHECK(ui.textBoxHeight(500.f, "one\ntwo", 40.f) == 96.f);  // (a line break: two lines)
    ui.end();
    ImGui::EndFrame();
}
