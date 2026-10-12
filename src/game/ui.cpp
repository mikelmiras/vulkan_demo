#include "game/ui.h"

#include <algorithm>

#include "game/fonts.h"

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
    return font->CalcTextSizeA(size * scale_, FLT_MAX, 0.f, text.c_str()).x / scale_;
}

void GameUI::text(float x, float y, const std::string& text, float size, Color color, Align align, const std::string& fontFile) {
    if (!dl_ || text.empty()) return;
    float w = textWidth(text, size, fontFile);
    if (align == Align::Center) x -= w * 0.5f;
    else if (align == Align::Right) x -= w;
    ImFont* font = gamefonts::font(fontFile);
    float px = size * scale_, off = std::max(1.f, size * 0.06f) * scale_;
    ImVec2 p = toScreen(x, y);
    dl_->AddText(font, px, ImVec2(p.x + off, p.y + off), rgba({0.f, 0.f, 0.f, color.a * 0.6f}), text.c_str());
    dl_->AddText(font, px, p, rgba(color), text.c_str());
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
