// game/color_glyphs: emoji tables read from fonts made here byte by byte (CBLC / CBDT, sbix, COLR / CPAL), the
// strike picked by size, damaged tables giving nothing
#include "test.h"

#include <cstring>
#include <map>
#include <string>

#include "game/color_glyphs.h"

namespace {

struct Out {
    std::string b;
    void u8(int v) { b += (char)(uint8_t)v; }
    void u16(int v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16((int)(v >> 16)); u16((int)(v & 0xFFFF)); }
    void tag(const char* t) { b.append(t, 4); }
    void raw(const std::string& s) { b += s; }
};

// An sfnt with these tables (directory, then each table 4-byte aligned)
std::string font(const std::map<std::string, std::string>& tables) {
    Out o;
    o.u32(0x00010000);
    o.u16((int)tables.size());
    o.u16(0), o.u16(0), o.u16(0);
    uint32_t at = 12 + 16 * (uint32_t)tables.size();
    std::string body;
    for (const auto& [tag, data] : tables) {
        o.tag(tag.c_str());
        o.u32(0);
        o.u32(at + (uint32_t)body.size());
        o.u32((uint32_t)data.size());
        body += data;
        while (body.size() % 4) body += '\0';
    }
    return o.b + body;
}

std::string maxp(int glyphs) {
    Out o;
    o.u32(0x00005000);
    o.u16(glyphs);
    return o.b;
}

// CBLC + CBDT: strikes of `ppems`, each with glyphs 1-3 (2 has no image) in image format 17
std::pair<std::string, std::string> cbdt(const std::vector<int>& ppems) {
    Out data;
    data.u16(3), data.u16(0);
    Out index;
    index.u16(3), index.u16(0);
    index.u32((uint32_t)ppems.size());
    std::vector<std::string> subtables;
    std::vector<uint32_t> arrayAt;
    uint32_t afterRecords = 8 + 48 * (uint32_t)ppems.size();
    std::string tail;
    for (int ppem : ppems) {
        // The images: glyph 1 and glyph 3 (each: small metrics, a length, "PNG" bytes)
        uint32_t imageData = (uint32_t)data.b.size();
        std::vector<uint32_t> offsets;
        for (int g = 1; g <= 3; ++g) {
            offsets.push_back((uint32_t)data.b.size() - imageData);
            if (g == 2) continue;
            data.u8(10), data.u8(12), data.u8(1), data.u8(9), data.u8(13);   // height, width, bearings, advance
            std::string png = "png of glyph " + std::to_string(g) + " at " + std::to_string(ppem);
            data.u32((uint32_t)png.size());
            data.raw(png);
        }
        offsets.push_back((uint32_t)data.b.size() - imageData);
        Out sub;   // index subtable array (one entry), then its subtable (format 1)
        uint32_t here = afterRecords + (uint32_t)tail.size();
        arrayAt.push_back(here);
        sub.u16(1), sub.u16(3), sub.u32(8);
        sub.u16(1), sub.u16(17), sub.u32(imageData);
        for (uint32_t off : offsets) sub.u32(off);
        tail += sub.b;
    }
    for (size_t s = 0; s < ppems.size(); ++s) {   // BitmapSize records
        index.u32(arrayAt[s]);
        index.u32(0);
        index.u32(1);
        index.u32(0);
        for (int k = 0; k < 24; ++k) index.u8(0);   // (line metrics)
        index.u16(1), index.u16(3);
        index.u8(ppems[s]), index.u8(ppems[s]), index.u8(32), index.u8(1);
    }
    index.raw(tail);
    return {index.b, data.b};
}

}  // namespace

TEST(color_glyphs_cbdt) {
    auto [cblc, data] = cbdt({20, 109});
    std::string f = font({{"CBDT", data}, {"CBLC", cblc}, {"maxp", maxp(4)}});
    const uint8_t* p = (const uint8_t*)f.data();
    CHECK(colorglyphs::hasColor(p, f.size(), 0));
    colorglyphs::Bitmap bm;
    REQUIRE(colorglyphs::findBitmap(p, f.size(), 0, 3, 64.f, bm));   // (64 pixels: the 109 strike, the nearest above)
    CHECK(bm.ppem == 109.f);
    CHECK(std::string((const char*)bm.png, bm.pngSize) == "png of glyph 3 at 109");
    CHECK(bm.height == 10.f && bm.width == 12.f && bm.bearingX == 1.f && bm.bearingY == 9.f && !bm.bottomOrigin);
    REQUIRE(colorglyphs::findBitmap(p, f.size(), 0, 1, 16.f, bm));
    CHECK(bm.ppem == 20.f);                                          // (16: the 20 strike)
    REQUIRE(colorglyphs::findBitmap(p, f.size(), 0, 1, 500.f, bm));
    CHECK(bm.ppem == 109.f);                                         // (bigger than any: the biggest)
    CHECK(!colorglyphs::findBitmap(p, f.size(), 0, 2, 64.f, bm));    // no image
    CHECK(!colorglyphs::findBitmap(p, f.size(), 0, 7, 64.f, bm));    // outside the strikes
    // Cut short: nothing, and no reads past the end
    for (size_t cut : {f.size() / 2, f.size() / 3, (size_t)40})
        CHECK(!colorglyphs::findBitmap(p, cut, 0, 3, 64.f, bm));
}

TEST(color_glyphs_sbix) {
    Out sbix;
    sbix.u16(1), sbix.u16(1), sbix.u32(1), sbix.u32(12);   // header, one strike at 12
    Out strike;
    strike.u16(64), strike.u16(72);
    std::string png = "an apple png";
    // glyph 0: nothing; 1: a PNG; 2: a duplicate of 1
    uint32_t start = 4 + 4 * 4;
    strike.u32(start), strike.u32(start);
    strike.u32(start + 8 + (uint32_t)png.size());
    strike.u32(start + 8 + (uint32_t)png.size() + 10);
    Out glyph;
    glyph.u16(3), glyph.u16((uint16_t)-5), glyph.tag("png "), glyph.raw(png);
    glyph.u16(0), glyph.u16(0), glyph.tag("dupe"), glyph.u16(1);
    sbix.raw(strike.b + glyph.b);
    std::string f = font({{"maxp", maxp(3)}, {"sbix", sbix.b}});
    const uint8_t* p = (const uint8_t*)f.data();
    colorglyphs::Bitmap bm;
    REQUIRE(colorglyphs::findBitmap(p, f.size(), 0, 1, 40.f, bm));
    CHECK(std::string((const char*)bm.png, bm.pngSize) == png);
    CHECK(bm.ppem == 64.f && bm.bearingX == 3.f && bm.bearingY == -5.f && bm.bottomOrigin);
    REQUIRE(colorglyphs::findBitmap(p, f.size(), 0, 2, 40.f, bm));   // (the duplicate: glyph 1's)
    CHECK(std::string((const char*)bm.png, bm.pngSize) == png);
    CHECK(!colorglyphs::findBitmap(p, f.size(), 0, 0, 40.f, bm));
    CHECK(!colorglyphs::findBitmap(p, f.size(), 0, 3, 40.f, bm));
}

TEST(color_glyphs_colr) {
    Out colr;   // glyph 5: glyph 6 in palette colour 1 under glyph 7 in the text's colour
    colr.u16(0), colr.u16(1), colr.u32(14), colr.u32(20), colr.u16(2);
    colr.u16(5), colr.u16(0), colr.u16(2);
    colr.u16(6), colr.u16(1), colr.u16(7), colr.u16(0xFFFF);
    Out cpal;   // one palette, two colours (BGRA)
    cpal.u16(0), cpal.u16(2), cpal.u16(1), cpal.u16(2), cpal.u32(14), cpal.u16(0);
    cpal.u8(0), cpal.u8(0), cpal.u8(255), cpal.u8(255);     // red
    cpal.u8(200), cpal.u8(100), cpal.u8(50), cpal.u8(128);  // (r 50, g 100, b 200, half)
    std::string f = font({{"COLR", colr.b}, {"CPAL", cpal.b}});
    const uint8_t* p = (const uint8_t*)f.data();
    std::vector<colorglyphs::Layer> layers;
    REQUIRE(colorglyphs::colrLayers(p, f.size(), 0, 5, layers));
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].glyph == 6 && !layers[0].foreground);
    CHECK(layers[0].rgba[0] == 50 && layers[0].rgba[1] == 100 && layers[0].rgba[2] == 200 && layers[0].rgba[3] == 128);
    CHECK(layers[1].glyph == 7 && layers[1].foreground);
    CHECK(!colorglyphs::colrLayers(p, f.size(), 0, 6, layers));   // (not a colour glyph)
    CHECK(!colorglyphs::colrLayers(p, f.size() - 6, 0, 5, layers));
}

TEST(color_glyphs_metrics) {
    Out head;
    for (int k = 0; k < 18; ++k) head.u8(0);
    head.u16(2048);
    Out hhea;
    for (int k = 0; k < 34; ++k) hhea.u8(0);
    hhea.u16(2);   // two long metrics; glyphs after them have the last advance
    Out hmtx;
    hmtx.u16(1000), hmtx.u16(0), hmtx.u16(2550), hmtx.u16(0);
    std::string f = font({{"head", head.b}, {"hhea", hhea.b}, {"hmtx", hmtx.b}, {"maxp", maxp(9)}});
    const uint8_t* p = (const uint8_t*)f.data();
    CHECK(colorglyphs::unitsPerEm(p, f.size(), 0) == 2048);
    CHECK(colorglyphs::glyphCount(p, f.size(), 0) == 9);
    CHECK(colorglyphs::advance(p, f.size(), 0, 0) == 1000);
    CHECK(colorglyphs::advance(p, f.size(), 0, 1) == 2550);
    CHECK(colorglyphs::advance(p, f.size(), 0, 7) == 2550);
    CHECK(!colorglyphs::hasColor(p, f.size(), 0));
}
