#include "game/ui.h"

#include <algorithm>

#include "game/fonts.h"
#include "game/glyph_font.h"
#include "game/line_break.h"
#include "game/prompts.h"

namespace {

// A run of text `px` pixels high: laid out by shaping when it needs it (game/text_shaping.h), else as Dear ImGui
// lays it out. Lines split at '\n'. Its width, and drawing it.
float runWidth(const std::string& text, float px, ImFont* font, const std::string& file) {
    if (!font) return 0.f;
    if (!shaping::needsShaping(text)) return font->CalcTextSizeA(px, FLT_MAX, 0.f, text.c_str()).x;
    float widest = 0.f;
    for (size_t at = 0; at <= text.size();) {
        size_t end = std::min(text.find('\n', at), text.size());
        const shaping::Shaped* sh = gamefonts::shaped(text.substr(at, end - at), file);
        widest = std::max(widest, sh ? sh->width * px * glyphfont::emScale()
                                     : font->CalcTextSizeA(px, FLT_MAX, 0.f, text.c_str() + at, text.c_str() + end).x);
        at = end + 1;
    }
    return widest;
}

// `shadow`: the drop shadow under it (colour glyphs, emoji, cast none: they'd show as pale shapes)
void drawRun(ImDrawList* dl, ImVec2 p, const std::string& text, float px, ImFont* font, const std::string& file, ImU32 col,
             bool shadow = false) {
    // (a locked atlas, whose renderer can't take new glyphs: as it is)
    if (!shaping::needsShaping(text) || (font && font->OwnerAtlas && font->OwnerAtlas->Locked)) {
        dl->AddText(font, px, p, col, text.c_str());
        return;
    }
    for (size_t at = 0; at <= text.size(); p.y += px) {
        size_t end = std::min(text.find('\n', at), text.size());
        std::string line = text.substr(at, end - at);
        at = end + 1;
        const shaping::Shaped* sh = gamefonts::shaped(line, file);
        if (!sh) {
            dl->AddText(font, px, p, col, line.c_str());
            continue;
        }
        const float em = px * glyphfont::emScale();   // (the em that makes it as big as Dear ImGui's text)
        for (const shaping::Glyph& g : sh->glyphs)   // (each face's glyphs through its font by glyph index)
            if (ImFont* gf = gamefonts::glyphFont(g.face)) {
                if (shadow) {
                    const ImFontGlyph* glyph = gf->GetFontBaked(em)->FindGlyphNoFallback(glyphfont::charFor(g.glyph));
                    if (glyph && glyph->Colored) continue;
                }
                gf->RenderChar(dl, em, ImVec2(p.x + g.x * em, p.y + g.y * em), col, glyphfont::charFor(g.glyph));
            }
    }
}

}  // namespace

void GameUI::begin(ImDrawList* dl, float x, float y, float w, float h, bool mouse) {
    dl_ = dl;
    x0_ = x;
    y0_ = y;
    scale_ = h > 1.f ? h / height() : 1.f;
    width_ = w / scale_;
    mouse_ = mouse;
    wantsCursor_ = false;
    if (dl_) dl_->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
}

void GameUI::end() {
    if (dl_) dl_->PopClipRect();
    dl_ = nullptr;
}

ImU32 GameUI::rgba(Color c) const {
    auto u = [](float v) { return (int)(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return IM_COL32(u(c.r), u(c.g), u(c.b), u(c.a));
}

void GameUI::rect(float x, float y, float w, float h, Color color, float rounding) {
    if (dl_) dl_->AddRectFilled(toScreen(x, y), toScreen(x + w, y + h), rgba(color), rounding * scale_);
}

void GameUI::border(float x, float y, float w, float h, Color color, float thickness, float rounding) {
    if (dl_) dl_->AddRect(toScreen(x, y), toScreen(x + w, y + h), rgba(color), rounding * scale_, 0, thickness * scale_);
}

float GameUI::textWidth(const std::string& text, float size, const std::string& fontFile) const {
    ImFont* font = gamefonts::font(fontFile);
    if (!font) return 0.f;
    if (!prompts::hasGlyphs(text)) return runWidth(text, size * scale_, font, fontFile) / scale_;
    float w = 0.f;   // (button prompts: the runs of text and the glyphs between them)
    for (const prompts::Piece& p : prompts::split(text))
        w += p.kind == prompts::Piece::Text ? runWidth(p.text, size * scale_, font, fontFile) : prompts::glyphWidth(p, size * scale_, font);
    return w / scale_;
}

void GameUI::text(float x, float y, const std::string& text, float size, Color color, Align align, const std::string& fontFile) {
    if (!dl_ || text.empty()) return;
    float w = textWidth(text, size, fontFile);
    if (align == Align::Center) x -= w * 0.5f;
    else if (align == Align::Right) x -= w;
    ImFont* font = gamefonts::font(fontFile);
    float px = size * scale_, off = std::max(1.f, size * 0.06f) * scale_;
    ImVec2 p = toScreen(x, y);
    if (!prompts::hasGlyphs(text)) {
        drawRun(dl_, ImVec2(p.x + off, p.y + off), text, px, font, fontFile, rgba({0.f, 0.f, 0.f, color.a * 0.6f}), true);
        drawRun(dl_, p, text, px, font, fontFile, rgba(color));
        return;
    }
    for (const prompts::Piece& piece : prompts::split(text)) {   // (the glyphs drawn in their own colours)
        if (piece.kind == prompts::Piece::Text) {
            drawRun(dl_, ImVec2(p.x + off, p.y + off), piece.text, px, font, fontFile, rgba({0.f, 0.f, 0.f, color.a * 0.6f}), true);
            drawRun(dl_, p, piece.text, px, font, fontFile, rgba(color));
            p.x += runWidth(piece.text, px, font, fontFile);
        } else {
            prompts::drawGlyph(dl_, font, p, px, piece, color.a);
            p.x += prompts::glyphWidth(piece, px, font);
        }
    }
}

void GameUI::bar(float x, float y, float w, float h, float fraction, Color fill, Color back) {
    rect(x, y, w, h, back, h * 0.25f);
    float f = std::clamp(fraction, 0.f, 1.f);
    if (f > 0.f) rect(x, y, w * f, h, fill, h * 0.25f);
}

void GameUI::image(const std::string& texture, float x, float y, float w, float h, Color tint) {
    if (!dl_ || !textures_) return;
    ImTextureID id = textures_(texture);
    if (id) dl_->AddImage(ImTextureRef(id), toScreen(x, y), toScreen(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), rgba(tint));
}

ImVec2 GameUI::mouse() const {
    ImVec2 m = ImGui::GetIO().MousePos;
    return {(m.x - x0_) / scale_, (m.y - y0_) / scale_};
}
bool GameUI::mouseDown() const { return mouse_ && ImGui::IsMouseDown(ImGuiMouseButton_Left); }
bool GameUI::mouseClicked() const { return mouse_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left); }

bool GameUI::hovered(float x, float y, float w, float h) const {
    if (!mouse_) return false;
    ImVec2 m = ImGui::GetIO().MousePos, a = toScreen(x, y), b = toScreen(x + w, y + h);
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
}

bool GameUI::button(float x, float y, float w, float h, const std::string& label, float textSize) {
    bool over = hovered(x, y, w, h);
    bool held = over && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    Color back = held ? Color{0.95f, 0.65f, 0.2f, 0.95f} : over ? Color{0.3f, 0.36f, 0.48f, 0.92f} : Color{0.12f, 0.14f, 0.2f, 0.85f};
    rect(x, y, w, h, back, 10.f);
    border(x, y, w, h, over ? Color{1.f, 0.8f, 0.4f, 1.f} : Color{1.f, 1.f, 1.f, 0.35f}, 2.f, 10.f);
    text(x + w * 0.5f, y + (h - textSize) * 0.5f, label, textSize, {}, Align::Center);
    return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

float GameUI::textBoxHeight(float w, const std::string& text, float size, const std::string& font) const {
    auto measure = [&](const std::string& piece) { return textWidth(piece, size, font); };
    return (float)linebreak::wrap(text, w, measure).size() * size * 1.2f;
}

float GameUI::textBox(float x, float y, float w, const std::string& text, float size, Color color, Align align,
                      const std::string& font) {
    auto measure = [&](const std::string& piece) { return textWidth(piece, size, font); };
    std::vector<linebreak::Line> lines = linebreak::wrap(text, w, measure);
    float ax = align == Align::Center ? x + w * 0.5f : align == Align::Right ? x + w : x;
    for (size_t i = 0; i < lines.size(); ++i)
        this->text(ax, y + (float)i * size * 1.2f, text.substr(lines[i].start, lines[i].end - lines[i].start), size, color, align, font);
    return (float)lines.size() * size * 1.2f;
}

std::vector<std::pair<size_t, float>> GameUI::caretStops(const std::string& text, float size, const std::string& font) const {
    std::vector<size_t> bytes;   // each character's first byte, then the end
    for (size_t i = 0; i < text.size(); ++i)
        if (((unsigned char)text[i] & 0xC0) != 0x80) bytes.push_back(i);
    bytes.push_back(text.size());
    std::vector<std::pair<size_t, float>> out;
    const shaping::Shaped* sh = shaping::needsShaping(text) ? gamefonts::shaped(text, font) : nullptr;
    std::vector<uint32_t> cps = shaping::decodeUtf8(text);
    if (sh && cps.size() + 1 == bytes.size()) {
        std::vector<float> stops = shaping::caretStops(cps, *sh);
        for (size_t i = 0; i < bytes.size(); ++i) out.push_back({bytes[i], stops[i] * size * glyphfont::emScale()});
        return out;
    }
    for (size_t b : bytes) out.push_back({b, textWidth(text.substr(0, b), size, font)});   // (left to right as it is)
    return out;
}
