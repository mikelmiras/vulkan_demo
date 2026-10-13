#include "game/glyph_font.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "game/color_glyphs.h"
#include "imgui_internal.h"
#include "stb_image.h"

// Our own (static) copies of Dear ImGui's stb_truetype and the rectangle packer it expects (as imgui_draw.cpp has
// them: its build keeps its functions to itself). Most of their functions go unused here.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#include "imstb_rectpack.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

namespace glyphfont {

namespace {

float g_baseline = 0.93f, g_emScale = 1.f;

struct SrcData {
    stbtt_fontinfo info;
    float scale = 1.f;   // (font units per pixel of text size)
    // Colour glyphs (game/color_glyphs.h); a font with bitmaps only (no outlines for stb_truetype) has just these
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t offset = 0;
    bool outlines = true, color = false;
    int glyphs = 0;
};

bool srcInit(ImFontAtlas*, ImFontConfig* src) {
    SrcData* d = IM_NEW(SrcData);
    d->data = (const uint8_t*)src->FontData;
    d->size = (size_t)src->FontDataSize;
    int offset = stbtt_GetFontOffsetForIndex((const unsigned char*)src->FontData, src->FontNo);
    if (offset < 0) {
        IM_DELETE(d);
        return false;
    }
    d->offset = (uint32_t)offset;
    d->color = colorglyphs::hasColor(d->data, d->size, d->offset);
    if (stbtt_InitFont(&d->info, (const unsigned char*)src->FontData, offset)) {
        // (scaled by its em, not its ascent-to-descent height: what shaping's positions are in)
        d->scale = stbtt_ScaleForMappingEmToPixels(&d->info, 1.f);
        d->glyphs = d->info.numGlyphs;
    } else if (d->color) {   // (an emoji font of bitmaps)
        d->outlines = false;
        d->scale = 1.f / (float)colorglyphs::unitsPerEm(d->data, d->size, d->offset);
        d->glyphs = colorglyphs::glyphCount(d->data, d->size, d->offset);
    } else {
        IM_DELETE(d);
        return false;
    }
    src->FontLoaderData = d;
    return true;
}

// A glyph in colour, as RGBA pixels `w` x `h` with its top-left corner at (x0, y0) raster pixels from its origin (y
// down): a bitmap (CBDT / sbix: its PNG, scaled to `pixels` an em) or COLR layers of outlines; false if it has none
bool colorGlyph(SrcData* d, int glyph, float pixels, float rasterX, float rasterY, std::vector<uint8_t>& rgba, int& w, int& h,
                float& x0, float& y0) {
    if (!d->color) return false;
    colorglyphs::Bitmap bm;
    if (colorglyphs::findBitmap(d->data, d->size, d->offset, (uint32_t)glyph, pixels, bm)) {
        int pw = 0, ph = 0, channels = 0;
        stbi_uc* png = stbi_load_from_memory(bm.png, (int)bm.pngSize, &pw, &ph, &channels, 4);
        if (!png) return false;
        float k = pixels / std::max(bm.ppem, 1.f);   // (strike pixels -> raster pixels)
        w = std::max(1, (int)std::lround((float)pw * k));
        h = std::max(1, (int)std::lround((float)ph * k));
        // Box filtered down (in premultiplied alpha), or nearest when growing it
        rgba.assign((size_t)w * (size_t)h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int sx0 = (int)((float)x / k), sx1 = std::max(sx0 + 1, (int)((float)(x + 1) / k));
                int sy0 = (int)((float)y / k), sy1 = std::max(sy0 + 1, (int)((float)(y + 1) / k));
                float sum[4] = {0, 0, 0, 0};
                int n = 0;
                for (int sy = sy0; sy < std::min(sy1, ph); ++sy)
                    for (int sx = sx0; sx < std::min(sx1, pw); ++sx, ++n) {
                        const stbi_uc* q = png + ((size_t)sy * (size_t)pw + (size_t)sx) * 4;
                        float a = q[3] / 255.f;
                        for (int c = 0; c < 3; ++c) sum[c] += q[c] * a;
                        sum[3] += a;
                    }
                uint8_t* o = &rgba[((size_t)y * (size_t)w + (size_t)x) * 4];
                if (n == 0 || sum[3] <= 0.f) continue;
                for (int c = 0; c < 3; ++c) o[c] = (uint8_t)std::min(255.f, sum[c] / sum[3] + 0.5f);
                o[3] = (uint8_t)std::min(255.f, sum[3] / (float)n * 255.f + 0.5f);
            }
        stbi_image_free(png);
        x0 = bm.bearingX * k;
        y0 = -(bm.bottomOrigin ? bm.bearingY * k + (float)h : bm.bearingY * k);
        return true;
    }
    std::vector<colorglyphs::Layer> layers;
    if (!d->outlines || !colorglyphs::colrLayers(d->data, d->size, d->offset, (uint32_t)glyph, layers)) return false;
    // COLR: every layer's outline over their joint box, each in its colour (the text's own: white, as colour
    // glyphs aren't tinted)
    int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
    for (const colorglyphs::Layer& l : layers) {
        int a, b, c, e;
        stbtt_GetGlyphBitmapBox(&d->info, (int)l.glyph, rasterX, rasterY, &a, &b, &c, &e);
        if (a == c || b == e) continue;
        bx0 = std::min(bx0, a), by0 = std::min(by0, b), bx1 = std::max(bx1, c), by1 = std::max(by1, e);
    }
    if (bx0 >= bx1 || by0 >= by1) return false;
    w = bx1 - bx0;
    h = by1 - by0;
    rgba.assign((size_t)w * (size_t)h * 4, 0);
    std::vector<uint8_t> mask;
    for (const colorglyphs::Layer& l : layers) {
        int a, b, c, e;
        stbtt_GetGlyphBitmapBox(&d->info, (int)l.glyph, rasterX, rasterY, &a, &b, &c, &e);
        if (a == c || b == e) continue;
        int lw = c - a, lh = e - b;
        mask.assign((size_t)lw * (size_t)lh, 0);
        stbtt_MakeGlyphBitmap(&d->info, mask.data(), lw, lh, lw, rasterX, rasterY, (int)l.glyph);
        const uint8_t* col = l.foreground ? (const uint8_t*)"\xff\xff\xff\xff" : l.rgba;
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                float m = mask[(size_t)y * (size_t)lw + (size_t)x] / 255.f * (col[3] / 255.f);
                if (m <= 0.f) continue;
                uint8_t* o = &rgba[((size_t)(y + b - by0) * (size_t)w + (size_t)(x + a - bx0)) * 4];
                float under = o[3] / 255.f, out = m + under * (1.f - m);   // (over: straight alpha)
                for (int k = 0; k < 3; ++k)
                    o[k] = (uint8_t)std::lround((col[k] * m + o[k] * under * (1.f - m)) / std::max(out, 1e-6f));
                o[3] = (uint8_t)std::lround(out * 255.f);
            }
    }
    x0 = (float)bx0;
    y0 = (float)by0;
    return true;
}

void srcDestroy(ImFontAtlas*, ImFontConfig* src) {
    IM_DELETE((SrcData*)src->FontLoaderData);
    src->FontLoaderData = nullptr;
}

bool containsGlyph(ImFontAtlas*, ImFontConfig* src, ImWchar c) {
    const SrcData* d = (const SrcData*)src->FontLoaderData;
    return d && c >= kFirstChar && (int)(c - kFirstChar) < d->glyphs;
}

bool bakedInit(ImFontAtlas*, ImFontConfig*, ImFontBaked* baked, void*) {
    baked->Ascent = std::ceil(g_baseline * baked->Size);
    baked->Descent = -std::ceil((1.2f - g_baseline) * baked->Size);
    return true;
}

// As Dear ImGui's stb_truetype loader, with the glyph's index for its character
bool loadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar c, ImFontGlyph* out, float* outAdvance) {
    SrcData* d = (SrcData*)src->FontLoaderData;
    if (!d || c < kFirstChar || (int)(c - kFirstChar) >= d->glyphs) return false;
    int glyph = (int)(c - kFirstChar);
    int oversampleH, oversampleV;
    ImFontAtlasBuildGetOversampleFactors(src, baked, &oversampleH, &oversampleV);
    const float layout = d->scale * baked->Size;
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
    {   // Colour (emoji): RGBA, not tinted by the text's colour
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        float cx0 = 0.f, cy0 = 0.f;
        const float pixels = baked->Size * density;
        if (colorGlyph(d, glyph, pixels, layout * density, layout * density, rgba, w, h, cx0, cy0)) {
            int adv = d->outlines ? 0 : colorglyphs::advance(d->data, d->size, d->offset, (uint32_t)glyph);
            if (d->outlines) stbtt_GetGlyphHMetrics(&d->info, glyph, &adv, nullptr);
            if (outAdvance) {
                *outAdvance = (float)adv * layout;
                return true;
            }
            out->Codepoint = c;
            out->AdvanceX = (float)adv * layout;
            ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, w, h);
            if (pack == ImFontAtlasRectId_Invalid) return false;
            ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack);
            const float inv = 1.f / density, top = IM_ROUND(baked->Ascent);
            out->X0 = cx0 * inv;
            out->Y0 = cy0 * inv + top;
            out->X1 = (cx0 + (float)r->w) * inv;
            out->Y1 = (cy0 + (float)r->h) * inv + top;
            out->Visible = true;
            out->Colored = true;
            out->PackId = pack;
            ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, rgba.data(), ImTextureFormat_RGBA32, w * 4);
            return true;
        }
    }
    if (!d->outlines) {   // (a bitmap font's glyph without an image, a space: its advance only, nothing drawn)
        float adv = (float)colorglyphs::advance(d->data, d->size, d->offset, (uint32_t)glyph) * layout;
        if (outAdvance) *outAdvance = adv;
        else out->Codepoint = c, out->AdvanceX = adv;
        return true;
    }
    const float rasterX = layout * density * (float)oversampleH, rasterY = layout * density * (float)oversampleV;
    int x0, y0, x1, y1, advance, lsb;
    stbtt_GetGlyphBitmapBoxSubpixel(&d->info, glyph, rasterX, rasterY, 0, 0, &x0, &y0, &x1, &y1);
    stbtt_GetGlyphHMetrics(&d->info, glyph, &advance, &lsb);
    if (outAdvance) {
        *outAdvance = (float)advance * layout;
        return true;
    }
    out->Codepoint = c;
    out->AdvanceX = (float)advance * layout;
    if (x0 == x1 || y0 == y1) return true;   // (a space, or a mark with no outline)
    const int w = x1 - x0 + oversampleH - 1, h = y1 - y0 + oversampleV - 1;
    ImFontAtlasRectId pack = ImFontAtlasPackAddRect(atlas, w, h);
    if (pack == ImFontAtlasRectId_Invalid) return false;
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack);
    stbtt_GetGlyphBitmapBox(&d->info, glyph, rasterX, rasterY, &x0, &y0, &x1, &y1);
    ImFontAtlasBuilder* builder = atlas->Builder;
    builder->TempBuffer.resize(w * h);
    unsigned char* pixels = builder->TempBuffer.Data;
    std::memset(pixels, 0, (size_t)w * (size_t)h);
    float subX, subY;
    stbtt_MakeGlyphBitmapSubpixelPrefilter(&d->info, pixels, w, h, w, rasterX, rasterY, 0, 0, oversampleH, oversampleV, &subX, &subY, glyph);
    const float rh = 1.f / ((float)oversampleH * density), rv = 1.f / ((float)oversampleV * density);
    const float offX = subX, offY = subY + IM_ROUND(baked->Ascent);   // (from the line's top: the shared baseline)
    out->X0 = (float)x0 * rh + offX;
    out->Y0 = (float)y0 * rv + offY;
    out->X1 = (float)(x0 + (int)r->w) * rh + offX;
    out->Y1 = (float)(y0 + (int)r->h) * rv + offY;
    out->Visible = true;
    out->PackId = pack;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, pixels, ImTextureFormat_Alpha8, w);
    return true;
}

}  // namespace

const ImFontLoader* loader() {
    static ImFontLoader l;
    l.Name = "glyph indices (stb_truetype)";
    l.FontSrcInit = srcInit;
    l.FontSrcDestroy = srcDestroy;
    l.FontSrcContainsGlyph = containsGlyph;
    l.FontBakedInit = bakedInit;
    l.FontBakedLoadGlyph = loadGlyph;
    return &l;
}

void setBaseline(float fraction) { g_baseline = fraction; }
void setEmScale(float scale) { g_emScale = scale; }
float emScale() { return g_emScale; }

float emScaleOf(const void* data, int index) {
    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex((const unsigned char*)data, index);
    if (offset < 0 || !stbtt_InitFont(&info, (const unsigned char*)data, offset)) return 1.f;
    return stbtt_ScaleForPixelHeight(&info, 1.f) / stbtt_ScaleForMappingEmToPixels(&info, 1.f);
}
float baseline() { return g_baseline; }

float ascentOf(const void* data, int index) {
    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex((const unsigned char*)data, index);
    if (offset < 0 || !stbtt_InitFont(&info, (const unsigned char*)data, offset)) return 0.8f;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    float em = stbtt_ScaleForMappingEmToPixels(&info, 1.f);
    return (float)ascent * em;
}

}  // namespace glyphfont
