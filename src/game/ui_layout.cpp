#include "game/ui_layout.h"

#include <algorithm>
#include <cmath>

#include "game/game.h"
#include "imgui.h"

namespace {

constexpr float kPad = 14.f;   // text inside a box: from its edge
const Color kAccent{0.98f, 0.68f, 0.22f, 1.f};

Color colorOf(Vec3 c, float a) { return {c.x, c.y, c.z, a}; }
Color lighter(Color c, float k) { return {c.r + (1.f - c.r) * k, c.g + (1.f - c.g) * k, c.b + (1.f - c.b) * k, c.a}; }

void appendUtf8(std::string& s, unsigned c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}
void popUtf8(std::string& s) {
    while (!s.empty()) {
        unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;   // (a whole character: back to its first byte)
    }
}

bool pressed(std::initializer_list<ImGuiKey> keys) {
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyPressed(k, true)) return true;
    return false;
}

void place(const Scene& scene, const Entity& e, const UIRect& parent, UILayout& out) {
    const UIElement& u = *e.ui;
    UIRect r;
    float x0 = parent.x + parent.w * u.anchorMin[0] + u.offsetMin[0], y0 = parent.y + parent.h * u.anchorMin[1] + u.offsetMin[1];
    float x1 = parent.x + parent.w * u.anchorMax[0] + u.offsetMax[0], y1 = parent.y + parent.h * u.anchorMax[1] + u.offsetMax[1];
    r = {x0, y0, std::max(x1 - x0, 0.f), std::max(y1 - y0, 0.f)};
    out.rects[e.id] = r;
    out.order.push_back(e.id);
    float cursor = r.y;   // (a list: children stacked down from its top)
    for (const Entity& c : scene.entities) {
        if (c.parent != e.id || !c.ui || !c.visible) continue;
        if (u.kind == UIElement::Kind::List) {
            const UIElement& cu = *c.ui;
            float h = std::max(cu.offsetMax[1] - cu.offsetMin[1], 1.f);
            UIRect slot{r.x, cursor, r.w, h};
            UIRect childParent = slot;   // (its own anchors / offsets then place it in its slot as if it filled it)
            UIElement filled = cu;
            filled.anchorMin[0] = filled.anchorMin[1] = 0.f;
            filled.anchorMax[0] = filled.anchorMax[1] = 1.f;
            filled.offsetMin[0] = filled.offsetMin[1] = filled.offsetMax[0] = filled.offsetMax[1] = 0.f;
            Entity tmp = c;
            tmp.ui = filled;
            place(scene, tmp, childParent, out);
            cursor += h + u.spacing;
        } else {
            place(scene, c, r, out);
        }
    }
}

}  // namespace

bool focusable(const UIElement& u) {
    return u.kind == UIElement::Kind::Button || u.kind == UIElement::Kind::Slider || u.kind == UIElement::Kind::Toggle ||
           u.kind == UIElement::Kind::TextField;
}

UILayout layoutUI(const Scene& scene, float width) {
    UILayout out;
    UIRect screen{0.f, 0.f, width, 1080.f};
    for (const Entity& e : scene.entities) {
        if (!e.ui || !scene.visibleInHierarchy(e.id)) continue;
        const Entity* p = scene.find(e.parent);
        if (p && p->ui) continue;   // (placed with its parent)
        place(scene, e, screen, out);
    }
    return out;
}

void UIController::frame(GameUI& ui, Scene& scene, const UILayout& layout, const Frame& f, std::vector<UIAction>& actions) {
    auto element = [&](EntityId id) -> UIElement* {
        Entity* e = scene.find(id);
        return e && e->ui ? &*e->ui : nullptr;
    };
    auto rectOf = [&](EntityId id) {
        auto it = layout.rects.find(id);
        return it != layout.rects.end() ? it->second : UIRect{};
    };
    std::vector<EntityId> focus;
    for (EntityId id : layout.order)
        if (const UIElement* u = element(id); u && focusable(*u)) focus.push_back(id);
    auto known = [&](EntityId id) { return std::find(focus.begin(), focus.end(), id) != focus.end(); };
    if (!known(focused_)) focused_ = kNoEntity;
    if (!known(editing_)) editing_ = kNoEntity;
    if (!known(dragging_)) dragging_ = kNoEntity;
    hasMenu_ = !focus.empty();
    auto fmt = [&](const std::string& s) { return f.format ? f.format(s) : s; };

    auto number = [&](const UIElement& u) { return f.state ? f.state->number(u.value, u.min) : u.min; };
    auto setNumber = [&](EntityId id, UIElement& u, float v) {
        v = std::clamp(v, std::min(u.min, u.max), std::max(u.min, u.max));
        if (!f.state || u.value.empty() || f.state->number(u.value, u.min) == v) return;
        f.state->setNumber(u.value, v);
        if (!u.action.empty()) actions.push_back({id, u.action, u.target, v, ""});
    };
    auto activate = [&](EntityId id) {
        UIElement* u = element(id);
        if (!u) return;
        switch (u->kind) {
            case UIElement::Kind::Button: actions.push_back({id, u->action, u->target, 0.f, ""}); break;
            case UIElement::Kind::Toggle: {
                bool on = !(f.state && f.state->flag(u->value));
                if (f.state && !u->value.empty()) f.state->setFlag(u->value, on);
                if (!u->action.empty()) actions.push_back({id, u->action, u->target, on ? 1.f : 0.f, ""});
                break;
            }
            case UIElement::Kind::TextField: editing_ = editing_ == id ? kNoEntity : id; break;
            default: break;
        }
    };

    if (f.interactive) {
        if (hasMenu_) ui.showCursor();
        // The mouse: the topmost element under it takes the focus; a press presses it, a drag moves a slider
        ImVec2 m = ui.mouse();
        EntityId under = kNoEntity;
        if (ui.mouseAvailable())
            for (size_t i = focus.size(); i-- > 0;)
                if (rectOf(focus[i]).contains(m.x, m.y)) {
                    under = focus[i];
                    break;
                }
        ImVec2 md = ImGui::GetIO().MouseDelta;
        if (under && (md.x != 0.f || md.y != 0.f)) focused_ = under;
        if (ui.mouseClicked()) {
            if (under) {
                focused_ = under;
                UIElement* u = element(under);
                if (u && u->kind == UIElement::Kind::Slider) dragging_ = under;
                else if (u && u->kind != UIElement::Kind::TextField) editing_ = kNoEntity;
                if (u && u->kind != UIElement::Kind::Slider && editing_ != under) activate(under);
            } else {
                editing_ = kNoEntity;
            }
        }
        if (!ui.mouseDown()) dragging_ = kNoEntity;
        if (UIElement* u = element(dragging_)) {
            UIRect r = rectOf(dragging_);
            float frac = r.w > 0.f ? std::clamp((m.x - r.x) / r.w, 0.f, 1.f) : 0.f;
            setNumber(dragging_, *u, u->min + (u->max - u->min) * frac);
        }
        // Keys and the gamepad
        if (f.keyboard && editing_ != kNoEntity) {
            UIElement* u = element(editing_);
            if (u && f.state && !u->value.empty()) {
                std::string text = f.state->text(u->value);
                std::string before = text;
                for (ImWchar c : ImGui::GetIO().InputQueueCharacters)
                    if (c >= 32 && c != 127) appendUtf8(text, c);
                if (pressed({ImGuiKey_Backspace})) popUtf8(text);
                if (text != before) f.state->setText(u->value, text);
            }
            if (pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape, ImGuiKey_GamepadFaceDown})) {
                if (u && !u->action.empty())
                    actions.push_back({editing_, u->action, u->target, 0.f, f.state ? f.state->text(u->value) : ""});
                editing_ = kNoEntity;
            }
        } else if (f.keyboard && !focus.empty()) {
            int dx = pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}) -
                     pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft});
            int dy = pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}) -
                     pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp});
            bool confirm = pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown});
            if (focused_ == kNoEntity && (dx || dy || confirm)) {
                focused_ = focus.front();   // (the first press only wakes the menu up)
            } else if (UIElement* u = element(focused_); u && u->kind == UIElement::Kind::Slider && dx) {
                setNumber(focused_, *u, number(*u) + (u->max - u->min) * 0.05f * (float)dx);
            } else if (dx || dy) {   // the nearest element that way (straight ahead counts more than off to the side)
                UIRect from = rectOf(focused_);
                float fx = from.x + from.w * 0.5f, fy = from.y + from.h * 0.5f, best = 1e30f;
                EntityId next = kNoEntity;
                for (EntityId id : focus) {
                    if (id == focused_) continue;
                    UIRect r = rectOf(id);
                    float ex = r.x + r.w * 0.5f - fx, ey = r.y + r.h * 0.5f - fy;
                    float along = ex * (float)dx + ey * (float)dy, across = std::fabs(ex * (float)dy - ey * (float)dx);
                    if (along <= 1.f) continue;
                    float score = along + across * 2.f;
                    if (score < best) best = score, next = id;
                }
                if (next != kNoEntity) focused_ = next;
            } else if (confirm) {
                activate(focused_);
            }
        }
    }

    // Draw, parents first
    for (EntityId id : layout.order) {
        const UIElement* u = element(id);
        if (!u) continue;
        UIRect r = rectOf(id);
        bool hot = f.interactive && id == focused_;
        Color back = colorOf(u->color, u->opacity);
        std::string text = fmt(u->text);
        float ty = r.y + (r.h - u->textSize) * 0.5f;
        auto label = [&](float x0, float x1, Color c) {
            if (text.empty()) return;
            GameUI::Align a = u->align == 0 ? GameUI::Align::Left : u->align == 2 ? GameUI::Align::Right : GameUI::Align::Center;
            float x = a == GameUI::Align::Left ? x0 + kPad : a == GameUI::Align::Right ? x1 - kPad : (x0 + x1) * 0.5f;
            ui.text(x, ty, text, u->textSize, c, a, u->font);
        };
        Color textColor = colorOf(u->textColor, 1.f);
        switch (u->kind) {
            case UIElement::Kind::Panel:
            case UIElement::Kind::List:
            case UIElement::Kind::Text:
                if (u->opacity > 0.f && u->kind != UIElement::Kind::Text) ui.rect(r.x, r.y, r.w, r.h, back, u->rounding);
                label(r.x, r.x + r.w, textColor);
                break;
            case UIElement::Kind::Image:
                if (!u->image.empty()) ui.image(u->image, r.x, r.y, r.w, r.h, {1.f, 1.f, 1.f, std::max(u->opacity, 0.f)});
                label(r.x, r.x + r.w, textColor);
                break;
            case UIElement::Kind::Button: {
                bool held = hot && ui.mouseDown() && ui.hovered(r.x, r.y, r.w, r.h);
                ui.rect(r.x, r.y, r.w, r.h, held ? kAccent : hot ? lighter(back, 0.25f) : back, u->rounding);
                if (hot) ui.border(r.x, r.y, r.w, r.h, kAccent, 3.f, u->rounding);
                label(r.x, r.x + r.w, held ? Color{0.1f, 0.08f, 0.05f, 1.f} : textColor);
                break;
            }
            case UIElement::Kind::Slider: {
                if (u->opacity > 0.f) ui.rect(r.x, r.y, r.w, r.h, back, u->rounding);
                float barX = text.empty() ? r.x + kPad : r.x + r.w * 0.45f, barW = r.x + r.w - kPad - barX;
                float range = u->max - u->min, frac = range != 0.f ? std::clamp((number(*u) - u->min) / range, 0.f, 1.f) : 0.f;
                float barH = std::max(r.h * 0.18f, 4.f);
                ui.bar(barX, r.y + (r.h - barH) * 0.5f, barW, barH, frac, hot ? kAccent : lighter(back, 0.6f),
                       {0.f, 0.f, 0.f, 0.45f});
                float knob = r.h * 0.55f;
                ui.rect(barX + barW * frac - knob * 0.5f, r.y + (r.h - knob) * 0.5f, knob, knob,
                        hot ? kAccent : Color{0.92f, 0.92f, 0.95f, 1.f}, knob * 0.5f);
                if (hot) ui.border(r.x, r.y, r.w, r.h, kAccent, 3.f, u->rounding);
                if (!text.empty()) ui.text(r.x + kPad, ty, text, u->textSize, textColor, GameUI::Align::Left, u->font);
                break;
            }
            case UIElement::Kind::Toggle: {
                if (u->opacity > 0.f) ui.rect(r.x, r.y, r.w, r.h, back, u->rounding);
                float box = r.h * 0.6f, bx = r.x + kPad, by = r.y + (r.h - box) * 0.5f;
                ui.border(bx, by, box, box, hot ? kAccent : Color{0.9f, 0.9f, 0.95f, 1.f}, 3.f, 4.f);
                if (f.state && f.state->flag(u->value)) ui.rect(bx + box * 0.2f, by + box * 0.2f, box * 0.6f, box * 0.6f, kAccent, 3.f);
                if (hot) ui.border(r.x, r.y, r.w, r.h, kAccent, 3.f, u->rounding);
                if (!text.empty()) ui.text(bx + box + kPad, ty, text, u->textSize, textColor, GameUI::Align::Left, u->font);
                break;
            }
            case UIElement::Kind::TextField: {
                ui.rect(r.x, r.y, r.w, r.h, back, u->rounding);
                bool editing = f.interactive && id == editing_;
                ui.border(r.x, r.y, r.w, r.h, editing || hot ? kAccent : Color{1.f, 1.f, 1.f, 0.35f}, 2.f, u->rounding);
                std::string value = f.state ? f.state->text(u->value) : "";
                bool placeholder = value.empty();
                std::string shown = placeholder ? text : value;
                Color c = placeholder ? Color{textColor.r, textColor.g, textColor.b, 0.45f} : textColor;
                ui.text(r.x + kPad, ty, shown, u->textSize, c, GameUI::Align::Left, u->font);
                if (editing && std::fmod(ImGui::GetTime(), 1.0) < 0.6) {   // the caret
                    float cx = r.x + kPad + (placeholder ? 0.f : ui.textWidth(value, u->textSize, u->font));
                    ui.rect(cx + 2.f, ty, 3.f, u->textSize, textColor);
                }
                break;
            }
        }
    }
}
