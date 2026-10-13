#include "game/glyph_font.h"

#include <cmath>
#include <cstring>

#include "imgui_internal.h"

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

float g_baseline = 0.93f;

struct SrcData {
    stbtt_fontinfo info;
    float scale = 1.f;   // (font units per pixel of text size)
};

bool srcInit(ImFontAtlas*, ImFontConfig* src) {
    SrcData* d = IM_NEW(SrcData);
    int offset = stbtt_GetFontOffsetForIndex((const unsigned char*)src->FontData, src->FontNo);
    if (offset < 0 || !stbtt_InitFont(&d->info, (const unsigned char*)src->FontData, offset)) {
        IM_DELETE(d);
        return false;
    }
    // (scaled by its em, not its ascent-to-descent height: what shaping's positions are in)
    d->scale = stbtt_ScaleForMappingEmToPixels(&d->info, 1.f);
    src->FontLoaderData = d;
    return true;
}

void srcDestroy(ImFontAtlas*, ImFontConfig* src) {
    IM_DELETE((SrcData*)src->FontLoaderData);
    src->FontLoaderData = nullptr;
}

bool containsGlyph(ImFontAtlas*, ImFontConfig* src, ImWchar c) {
    const SrcData* d = (const SrcData*)src->FontLoaderData;
    return d && c >= 1 && (int)c - 1 < d->info.numGlyphs;
}

bool bakedInit(ImFontAtlas*, ImFontConfig*, ImFontBaked* baked, void*) {
    baked->Ascent = std::ceil(g_baseline * baked->Size);
    baked->Descent = -std::ceil((1.2f - g_baseline) * baked->Size);
    return true;
}

// As Dear ImGui's stb_truetype loader, with the glyph's index for its character
bool loadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar c, ImFontGlyph* out, float* outAdvance) {
    SrcData* d = (SrcData*)src->FontLoaderData;
    if (!d || c < 1 || (int)c - 1 >= d->info.numGlyphs) return false;
    int glyph = (int)c - 1;
    int oversampleH, oversampleV;
    ImFontAtlasBuildGetOversampleFactors(src, baked, &oversampleH, &oversampleV);
    const float layout = d->scale * baked->Size;
    const float density = src->RasterizerDensity * baked->RasterizerDensity;
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
