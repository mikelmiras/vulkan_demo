// Colour glyph tables: OpenType's CBLC / CBDT, sbix and COLR / CPAL (version 0), read with bounds checks (a damaged
// font gives no colour glyph, never a read outside it). See color_glyphs.h.
#include "game/color_glyphs.h"

#include <cstring>

namespace colorglyphs {

namespace {

// Big-endian reads within [0, size)
struct Bytes {
    const uint8_t* data;
    size_t size;
    bool has(size_t at, size_t n) const { return at <= size && n <= size - at; }
    uint8_t u8(size_t at) const { return has(at, 1) ? data[at] : 0; }
    int8_t i8(size_t at) const { return (int8_t)u8(at); }
    uint16_t u16(size_t at) const { return has(at, 2) ? (uint16_t)(data[at] << 8 | data[at + 1]) : 0; }
    int16_t i16(size_t at) const { return (int16_t)u16(at); }
    uint32_t u32(size_t at) const {
        return has(at, 4) ? (uint32_t)data[at] << 24 | (uint32_t)data[at + 1] << 16 | (uint32_t)data[at + 2] << 8 | data[at + 3] : 0;
    }
};

// CBDT's glyph metrics (small: 5 bytes, big: 8; the vertical half of big ones isn't needed)
void metrics(const Bytes& b, size_t at, Bitmap& out) {
    out.height = b.u8(at);
    out.width = b.u8(at + 1);
    out.bearingX = b.i8(at + 2);
    out.bearingY = b.i8(at + 3);
}

// The glyph's image in CBDT: its offset there and length, from a CBLC index subtable (formats 1 to 5)
bool locate(const Bytes& b, size_t sub, uint16_t first, uint16_t last, uint32_t glyph, uint32_t& offset, uint32_t& length,
            uint16_t& imageFormat, bool& haveMetrics, Bitmap& out) {
    uint16_t indexFormat = b.u16(sub);
    imageFormat = b.u16(sub + 2);
    uint32_t imageData = b.u32(sub + 4);
    size_t at = sub + 8;
    uint32_t k = glyph - first;
    haveMetrics = false;
    switch (indexFormat) {
        case 1: {   // an offset per glyph (4 bytes)
            uint32_t a = b.u32(at + 4 * (size_t)k), z = b.u32(at + 4 * ((size_t)k + 1));
            if (z <= a) return false;
            offset = imageData + a;
            length = z - a;
            return true;
        }
        case 3: {   // the same, 2 bytes each
            uint32_t a = b.u16(at + 2 * (size_t)k), z = b.u16(at + 2 * ((size_t)k + 1));
            if (z <= a) return false;
            offset = imageData + a;
            length = z - a;
            return true;
        }
        case 2: {   // every glyph's image the same size, one set of metrics
            uint32_t imageSize = b.u32(at);
            metrics(b, at + 4, out);
            haveMetrics = true;
            offset = imageData + k * imageSize;
            length = imageSize;
            return imageSize > 0;
        }
        case 4: {   // (glyph, offset) pairs for the glyphs there are
            uint32_t n = b.u32(at);
            for (uint32_t i = 0; i < n && b.has(at + 4 + 4 * (size_t)i, 8); ++i)
                if (b.u16(at + 4 + 4 * (size_t)i) == glyph) {
                    uint32_t a = b.u16(at + 4 + 4 * (size_t)i + 2), z = b.u16(at + 4 + 4 * ((size_t)i + 1) + 2);
                    if (z <= a) return false;
                    offset = imageData + a;
                    length = z - a;
                    return true;
                }
            return false;
        }
        case 5: {   // one size and metrics, a list of the glyphs there are
            uint32_t imageSize = b.u32(at);
            metrics(b, at + 4, out);
            haveMetrics = true;
            uint32_t n = b.u32(at + 12);
            for (uint32_t i = 0; i < n && b.has(at + 16 + 2 * (size_t)i, 2); ++i)
                if (b.u16(at + 16 + 2 * (size_t)i) == glyph) {
                    offset = imageData + i * imageSize;
                    length = imageSize;
                    return imageSize > 0;
                }
            return false;
        }
        default: (void)last; return false;
    }
}

bool cbdt(const Bytes& b, uint32_t fontOffset, uint32_t glyph, float pixels, Bitmap& out) {
    uint32_t cblc, cblcLength, data, dataLength;
    if (!findTable(b.data, b.size, fontOffset, "CBLC", cblc, cblcLength) || !findTable(b.data, b.size, fontOffset, "CBDT", data, dataLength))
        return false;
    Bytes index{b.data + cblc, cblcLength}, images{b.data + data, dataLength};
    uint32_t sizes = index.u32(4);
    // The strike: the smallest at least `pixels` big that has the glyph, else the biggest that has it
    long best = -1;
    int bestPpem = 0;
    for (uint32_t s = 0; s < sizes && index.has(8 + 48 * (size_t)s, 48); ++s) {
        size_t rec = 8 + 48 * (size_t)s;
        uint16_t start = index.u16(rec + 40), end = index.u16(rec + 42);
        int ppem = index.u8(rec + 45);
        if (glyph < start || glyph > end) continue;
        bool better = best < 0 || (bestPpem < pixels ? ppem > bestPpem : ppem >= pixels && ppem < bestPpem);
        if (better) best = (long)s, bestPpem = ppem;
    }
    if (best < 0) return false;
    size_t rec = 8 + 48 * (size_t)best;
    uint32_t arrayOffset = index.u32(rec), count = index.u32(rec + 8);
    for (uint32_t i = 0; i < count && index.has(arrayOffset + 8 * (size_t)i, 8); ++i) {
        size_t entry = arrayOffset + 8 * (size_t)i;
        uint16_t first = index.u16(entry), last = index.u16(entry + 2);
        if (glyph < first || glyph > last) continue;
        uint32_t offset = 0, length = 0;
        uint16_t format = 0;
        bool haveMetrics = false;
        Bitmap bm;
        if (!locate(index, arrayOffset + index.u32(entry + 4), first, last, glyph, offset, length, format, haveMetrics, bm))
            return false;
        if (!images.has(offset, length)) return false;
        size_t png = offset;
        uint32_t pngSize = 0;
        if (format == 17) {        // small metrics, length, PNG
            metrics(images, offset, bm);
            pngSize = images.u32(offset + 5);
            png = offset + 9;
        } else if (format == 18) { // big metrics, length, PNG
            metrics(images, offset, bm);
            pngSize = images.u32(offset + 8);
            png = offset + 12;
        } else if (format == 19 && haveMetrics) {   // length, PNG (metrics in the index)
            pngSize = images.u32(offset);
            png = offset + 4;
        } else {
            return false;   // (uncompressed bitmaps: not for colour)
        }
        if (pngSize == 0 || !images.has(png, pngSize) || png + pngSize > (size_t)offset + length) return false;
        out = bm;
        out.png = images.data + png;
        out.pngSize = pngSize;
        out.ppem = (float)bestPpem;
        return true;
    }
    return false;
}

bool sbix(const Bytes& b, uint32_t fontOffset, uint32_t glyph, float pixels, Bitmap& out) {
    uint32_t at, length;
    if (!findTable(b.data, b.size, fontOffset, "sbix", at, length)) return false;
    Bytes t{b.data + at, length};
    int glyphs = glyphCount(b.data, b.size, fontOffset);
    if (glyph >= (uint32_t)glyphs) return false;
    uint32_t strikes = t.u32(4);
    long best = -1;
    int bestPpem = 0;
    for (uint32_t s = 0; s < strikes; ++s) {
        uint32_t strike = t.u32(8 + 4 * (size_t)s);
        int ppem = t.u16(strike);
        bool better = best < 0 || (bestPpem < pixels ? ppem > bestPpem : ppem >= pixels && ppem < bestPpem);
        if (better && t.has(strike, 4 + 4 * ((size_t)glyphs + 1))) best = (long)strike, bestPpem = ppem;
    }
    if (best < 0) return false;
    size_t strike = (size_t)best;
    for (int hop = 0; hop < 2; ++hop) {   // ('dupe': another glyph's image, once)
        uint32_t a = t.u32(strike + 4 + 4 * (size_t)glyph), z = t.u32(strike + 4 + 4 * ((size_t)glyph + 1));
        if (z <= a + 8 || !t.has(strike + a, z - a)) return false;
        size_t data = strike + a;
        if (std::memcmp(t.data + data + 4, "dupe", 4) == 0) {
            glyph = t.u16(data + 8);
            if (glyph >= (uint32_t)glyphs) return false;
            continue;
        }
        if (std::memcmp(t.data + data + 4, "png ", 4) != 0) return false;
        out = {};
        out.png = t.data + data + 8;
        out.pngSize = z - a - 8;
        out.ppem = (float)bestPpem;
        out.bearingX = t.i16(data);
        out.bearingY = t.i16(data + 2);   // (the image's bottom edge: its top is this plus its height)
        out.bottomOrigin = true;
        return true;
    }
    return false;
}

}  // namespace

bool findTable(const uint8_t* data, size_t size, uint32_t fontOffset, const char tag[4], uint32_t& offset, uint32_t& length) {
    Bytes b{data, size};
    uint16_t tables = b.u16((size_t)fontOffset + 4);
    for (uint16_t i = 0; i < tables; ++i) {
        size_t rec = (size_t)fontOffset + 12 + 16 * (size_t)i;
        if (!b.has(rec, 16)) return false;
        if (std::memcmp(data + rec, tag, 4) != 0) continue;
        offset = b.u32(rec + 8);
        length = b.u32(rec + 12);
        return b.has(offset, length);
    }
    return false;
}

bool findBitmap(const uint8_t* data, size_t size, uint32_t fontOffset, uint32_t glyph, float pixels, Bitmap& out) {
    Bytes b{data, size};
    return cbdt(b, fontOffset, glyph, pixels, out) || sbix(b, fontOffset, glyph, pixels, out);
}

bool colrLayers(const uint8_t* data, size_t size, uint32_t fontOffset, uint32_t glyph, std::vector<Layer>& out) {
    out.clear();
    uint32_t colr, colrLength, cpal, cpalLength;
    if (!findTable(data, size, fontOffset, "COLR", colr, colrLength) || !findTable(data, size, fontOffset, "CPAL", cpal, cpalLength))
        return false;
    Bytes c{data + colr, colrLength}, p{data + cpal, cpalLength};
    uint16_t bases = c.u16(2), layers = c.u16(12);
    uint32_t baseAt = c.u32(4), layerAt = c.u32(8);
    // Palette 0's colours (BGRA records)
    uint16_t entries = p.u16(2), colours = p.u16(6);
    uint32_t recordsAt = p.u32(8);
    uint16_t firstColour = p.u16(12);
    for (uint16_t i = 0; i < bases; ++i) {   // (sorted by glyph: a short walk)
        size_t rec = baseAt + 6 * (size_t)i;
        if (!c.has(rec, 6)) return false;
        if (c.u16(rec) != glyph) continue;
        uint16_t first = c.u16(rec + 2), count = c.u16(rec + 4);
        for (uint16_t k = 0; k < count; ++k) {
            uint32_t li = (uint32_t)first + k;
            if (li >= layers || !c.has(layerAt + 4 * (size_t)li, 4)) return false;
            Layer l;
            l.glyph = c.u16(layerAt + 4 * (size_t)li);
            uint16_t index = c.u16(layerAt + 4 * (size_t)li + 2);
            if (index == 0xFFFF) {
                l.foreground = true;
            } else {
                uint32_t colour = (uint32_t)firstColour + index;
                if (index >= entries || colour >= colours || !p.has(recordsAt + 4 * (size_t)colour, 4)) return false;
                const uint8_t* bgra = p.data + recordsAt + 4 * (size_t)colour;
                l.rgba[0] = bgra[2], l.rgba[1] = bgra[1], l.rgba[2] = bgra[0], l.rgba[3] = bgra[3];
            }
            out.push_back(l);
        }
        return !out.empty();
    }
    return false;
}

bool hasColor(const uint8_t* data, size_t size, uint32_t fontOffset) {
    uint32_t o, l;
    return findTable(data, size, fontOffset, "CBDT", o, l) || findTable(data, size, fontOffset, "sbix", o, l) ||
           findTable(data, size, fontOffset, "COLR", o, l);
}

int glyphCount(const uint8_t* data, size_t size, uint32_t fontOffset) {
    uint32_t at, length;
    if (!findTable(data, size, fontOffset, "maxp", at, length)) return 0;
    return Bytes{data + at, length}.u16(4);
}

int unitsPerEm(const uint8_t* data, size_t size, uint32_t fontOffset) {
    uint32_t at, length;
    if (!findTable(data, size, fontOffset, "head", at, length)) return 1000;
    int em = Bytes{data + at, length}.u16(18);
    return em > 0 ? em : 1000;
}

int advance(const uint8_t* data, size_t size, uint32_t fontOffset, uint32_t glyph) {
    uint32_t hhea, hheaLength, hmtx, hmtxLength;
    if (!findTable(data, size, fontOffset, "hhea", hhea, hheaLength) || !findTable(data, size, fontOffset, "hmtx", hmtx, hmtxLength))
        return 0;
    uint16_t metrics = Bytes{data + hhea, hheaLength}.u16(34);
    if (metrics == 0) return 0;
    uint32_t k = glyph < metrics ? glyph : metrics - 1u;   // (past the last: the last one's advance)
    return Bytes{data + hmtx, hmtxLength}.u16(4 * (size_t)k);
}

}  // namespace colorglyphs
