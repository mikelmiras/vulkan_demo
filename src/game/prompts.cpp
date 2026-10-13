#include "game/prompts.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>

namespace prompts {

namespace {

constexpr uint32_t kPadBase = 0xE000, kKeyOpen = 0xE100, kKeyClose = 0xE101;

std::string utf8(uint32_t cp) {   // (three bytes: the private-use range)
    std::string s;
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
    return s;
}

// The private-use character at `i` (three bytes from 0xEE), or 0
uint32_t privateAt(const std::string& s, size_t i) {
    if (i + 2 >= s.size() || (uint8_t)s[i] != 0xEE) return 0;
    uint8_t b1 = (uint8_t)s[i + 1], b2 = (uint8_t)s[i + 2];
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
    uint32_t cp = 0xE000 | ((uint32_t)(b1 & 0x3F) << 6) | (b2 & 0x3F);
    return (cp >= kPadBase && cp < kPadBase + 4 * 32) || cp == kKeyOpen || cp == kKeyClose ? cp : 0;
}

bool isFace(ImGuiKey k) {
    return k == ImGuiKey_GamepadFaceDown || k == ImGuiKey_GamepadFaceRight || k == ImGuiKey_GamepadFaceLeft || k == ImGuiKey_GamepadFaceUp;
}
bool isDpad(ImGuiKey k) { return k >= ImGuiKey_GamepadDpadLeft && k <= ImGuiKey_GamepadDpadDown; }
bool isStick(ImGuiKey k) { return k == ImGuiKey_GamepadL3 || k == ImGuiKey_GamepadR3 || k >= ImGuiKey_GamepadLStickLeft; }

// A label for a pill-shaped glyph (shoulders, triggers, menu buttons) or inside a stick's circle
std::string shortLabel(PadType type, ImGuiKey k) {
    bool ps = type == PadType::PlayStation, sw = type == PadType::Switch;
    switch (k) {
    case ImGuiKey_GamepadL1: return ps ? "L1" : sw ? "L" : "LB";
    case ImGuiKey_GamepadR1: return ps ? "R1" : sw ? "R" : "RB";
    case ImGuiKey_GamepadL2: return ps ? "L2" : sw ? "ZL" : "LT";
    case ImGuiKey_GamepadR2: return ps ? "R2" : sw ? "ZR" : "RT";
    case ImGuiKey_GamepadStart: return ps ? "Options" : sw ? "+" : "Menu";
    case ImGuiKey_GamepadBack: return ps ? "Create" : sw ? "-" : "View";
    case ImGuiKey_GamepadL3: return ps ? "L3" : "LS";
    case ImGuiKey_GamepadR3: return ps ? "R3" : "RS";
    default: return k >= ImGuiKey_GamepadRStickLeft ? (ps ? "R" : "RS") : k >= ImGuiKey_GamepadLStickLeft ? (ps ? "L" : "LS") : "";
    }
}

// Face buttons: Xbox (and others) A B X Y from the bottom round; Switch B A Y X; PlayStation shapes
const char* faceLetter(PadType type, ImGuiKey k) {
    bool sw = type == PadType::Switch;
    switch (k) {
    case ImGuiKey_GamepadFaceDown: return sw ? "B" : "A";
    case ImGuiKey_GamepadFaceRight: return sw ? "A" : "B";
    case ImGuiKey_GamepadFaceLeft: return sw ? "Y" : "X";
    case ImGuiKey_GamepadFaceUp: return sw ? "X" : "Y";
    default: return "";
    }
}

ImU32 color(float r, float g, float b, float a) {
    return IM_COL32((int)(r * 255.f), (int)(g * 255.f), (int)(b * 255.f), (int)(std::clamp(a, 0.f, 1.f) * 255.f));
}

}  // namespace

std::string padGlyph(PadType type, ImGuiKey key) {
    if (!isGamepadKey(key)) return {};
    return utf8(kPadBase + (uint32_t)type * 32 + (uint32_t)(key - ImGuiKey_GamepadStart));
}

std::string keyCap(const std::string& label) { return utf8(kKeyOpen) + label + utf8(kKeyClose); }

std::string keyLabel(ImGuiKey key) {
    std::string name = keyName(key);
    if (name == "Escape") return "Esc";
    if (name.size() > 5 && name.compare(name.size() - 5, 5, "Arrow") == 0) return name.substr(0, name.size() - 5);
    if (name.rfind("Keypad", 0) == 0) return "Num " + name.substr(6);
    std::string out;   // "LeftShift" -> "Left Shift"
    for (size_t i = 0; i < name.size(); ++i) {
        if (i > 0 && std::isupper((unsigned char)name[i]) && std::islower((unsigned char)name[i - 1])) out += ' ';
        out += name[i];
    }
    return out;
}

std::string mouseLabel(int button) {
    switch (button) {
    case 0: return "Left click";
    case 1: return "Right click";
    case 2: return "Middle click";
    default: return "Mouse " + std::to_string(button + 1);
    }
}

std::string padLabel(PadType type, ImGuiKey key) {
    if (isFace(key)) {
        if (type == PadType::PlayStation)
            return key == ImGuiKey_GamepadFaceDown ? "Cross" : key == ImGuiKey_GamepadFaceRight ? "Circle"
                   : key == ImGuiKey_GamepadFaceLeft ? "Square" : "Triangle";
        return faceLetter(type, key);
    }
    if (isDpad(key)) {
        const char* dir = key == ImGuiKey_GamepadDpadLeft ? "left" : key == ImGuiKey_GamepadDpadRight ? "right"
                          : key == ImGuiKey_GamepadDpadUp ? "up" : "down";
        return std::string("D-pad ") + dir;
    }
    if (key >= ImGuiKey_GamepadLStickLeft) {
        static const char* dirs[] = {"left", "right", "up", "down"};
        int i = (key - ImGuiKey_GamepadLStickLeft) % 4;
        return std::string(key >= ImGuiKey_GamepadRStickLeft ? "Right stick " : "Left stick ") + dirs[i];
    }
    return shortLabel(type, key);
}

std::string forAction(const InputAction& action, Device device, PadType type) {
    for (ImGuiKey k : action.keys)
        if ((device == Device::Pad) == isGamepadKey(k)) return device == Device::Pad ? padGlyph(type, k) : keyCap(keyLabel(k));
    if (device == Device::Keyboard && action.mouseButton >= 0) return keyCap(mouseLabel(action.mouseButton));
    return {};
}

std::string wordsForAction(const InputAction& action, Device device, PadType type) {
    for (ImGuiKey k : action.keys)
        if ((device == Device::Pad) == isGamepadKey(k)) return device == Device::Pad ? padLabel(type, k) : keyLabel(k);
    if (device == Device::Keyboard && action.mouseButton >= 0) return mouseLabel(action.mouseButton);
    return {};
}

bool hasGlyphs(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i)
        if (privateAt(text, i)) return true;
    return false;
}

std::vector<Piece> split(const std::string& text) {
    std::vector<Piece> out;
    std::string run;
    auto flush = [&] {
        if (!run.empty()) out.push_back({Piece::Text, run});
        run.clear();
    };
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = privateAt(text, i);
        if (!cp) {
            run += text[i++];
            continue;
        }
        if (cp >= kPadBase && cp < kPadBase + 4 * 32) {
            uint32_t n = cp - kPadBase;
            flush();
            Piece p;
            p.kind = Piece::Pad;
            p.type = (PadType)(n / 32);
            p.key = (ImGuiKey)(ImGuiKey_GamepadStart + (int)(n % 32));
            if (isGamepadKey(p.key)) out.push_back(p);
            i += 3;
        } else if (cp == kKeyOpen) {
            size_t end = text.find(utf8(kKeyClose), i + 3);
            flush();
            Piece p;
            p.kind = Piece::Key;
            p.text = text.substr(i + 3, (end == std::string::npos ? text.size() : end) - (i + 3));
            out.push_back(p);
            i = end == std::string::npos ? text.size() : end + 3;
        } else {
            i += 3;   // (a stray closing mark)
        }
    }
    flush();
    return out;
}

float glyphWidth(const Piece& p, float size, ImFont* font) {
    if (p.kind == Piece::Key) {
        float label = font ? font->CalcTextSizeA(size * 0.62f, FLT_MAX, 0.f, p.text.c_str()).x : size;
        return std::max(label + size * 0.5f, size * 0.95f) + size * 0.12f;
    }
    if (p.kind != Piece::Pad) return 0.f;
    if (isFace(p.key) || isDpad(p.key) || isStick(p.key)) return size * 1.05f;
    std::string label = shortLabel(p.type, p.key);   // (pills)
    float w = font ? font->CalcTextSizeA(size * 0.5f, FLT_MAX, 0.f, label.c_str()).x : size;
    return std::max(w + size * 0.45f, size * 1.05f) + size * 0.12f;
}

void drawGlyph(ImDrawList* dl, ImFont* font, ImVec2 pos, float size, const Piece& p, float alpha) {
    if (!dl) return;
    float w = glyphWidth(p, size, font), gap = size * 0.06f;
    float cy = pos.y + size * 0.55f;   // (the middle of a line of text)
    auto centred = [&](const std::string& label, float textSize, ImVec2 c, ImU32 col) {
        if (!font || label.empty()) return;
        ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0.f, label.c_str());
        dl->AddText(font, textSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, label.c_str());
    };
    ImU32 dark = color(0.12f, 0.13f, 0.16f, 0.92f * alpha), light = color(0.95f, 0.95f, 0.97f, alpha);
    ImU32 rim = color(1.f, 1.f, 1.f, 0.55f * alpha);
    if (p.kind == Piece::Key) {   // a keycap: a light box with the label
        ImVec2 a(pos.x + gap, cy - size * 0.47f), b(pos.x + w - gap, cy + size * 0.47f);
        dl->AddRectFilled(a, b, color(0.9f, 0.9f, 0.93f, 0.95f * alpha), size * 0.16f);
        dl->AddRect(a, b, color(0.35f, 0.36f, 0.42f, alpha), size * 0.16f, 0, std::max(1.f, size * 0.05f));
        centred(p.text, size * 0.62f, ImVec2((a.x + b.x) * 0.5f, cy), color(0.1f, 0.1f, 0.12f, alpha));
        return;
    }
    ImVec2 c(pos.x + w * 0.5f, cy);
    float r = size * 0.47f;
    if (isFace(p.key)) {
        dl->AddCircleFilled(c, r, dark, 32);
        dl->AddCircle(c, r, rim, 32, std::max(1.f, size * 0.04f));
        if (p.type == PadType::PlayStation) {   // the shapes, in their colours
            float s = r * 0.5f, t = std::max(1.5f, size * 0.08f);
            if (p.key == ImGuiKey_GamepadFaceDown) {
                ImU32 blue = color(0.49f, 0.7f, 0.98f, alpha);
                dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), blue, t);
                dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), blue, t);
            } else if (p.key == ImGuiKey_GamepadFaceRight) {
                dl->AddCircle(c, s * 1.05f, color(0.98f, 0.42f, 0.47f, alpha), 24, t);
            } else if (p.key == ImGuiKey_GamepadFaceLeft) {
                dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f), color(0.92f, 0.58f, 0.85f, alpha), 0.f, 0, t);
            } else {
                dl->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x + s * 1.05f, c.y + s * 0.75f), ImVec2(c.x - s * 1.05f, c.y + s * 0.75f),
                                color(0.3f, 0.85f, 0.7f, alpha), t);
            }
        } else {
            ImU32 col = light;
            if (p.type == PadType::Xbox) {   // (Xbox's letters in their colours)
                col = p.key == ImGuiKey_GamepadFaceDown ? color(0.45f, 0.82f, 0.3f, alpha) : p.key == ImGuiKey_GamepadFaceRight ? color(0.95f, 0.3f, 0.3f, alpha)
                      : p.key == ImGuiKey_GamepadFaceLeft ? color(0.3f, 0.6f, 1.f, alpha) : color(1.f, 0.82f, 0.2f, alpha);
            }
            centred(faceLetter(p.type, p.key), size * 0.62f, c, col);
        }
        return;
    }
    if (isDpad(p.key)) {   // a cross with the pressed arm lit
        float arm = r * 0.36f;
        dl->AddRectFilled(ImVec2(c.x - arm, c.y - r), ImVec2(c.x + arm, c.y + r), dark, size * 0.06f);
        dl->AddRectFilled(ImVec2(c.x - r, c.y - arm), ImVec2(c.x + r, c.y + arm), dark, size * 0.06f);
        ImVec2 a, b;
        if (p.key == ImGuiKey_GamepadDpadUp) a = {c.x - arm, c.y - r}, b = {c.x + arm, c.y - arm};
        else if (p.key == ImGuiKey_GamepadDpadDown) a = {c.x - arm, c.y + arm}, b = {c.x + arm, c.y + r};
        else if (p.key == ImGuiKey_GamepadDpadLeft) a = {c.x - r, c.y - arm}, b = {c.x - arm, c.y + arm};
        else a = {c.x + arm, c.y - arm}, b = {c.x + r, c.y + arm};
        dl->AddRectFilled(a, b, light, size * 0.06f);
        return;
    }
    if (isStick(p.key)) {   // a ring with its name, and an arrow for a direction
        dl->AddCircleFilled(c, r, dark, 32);
        dl->AddCircle(c, r * 0.92f, rim, 32, std::max(1.f, size * 0.06f));
        centred(shortLabel(p.type, p.key), size * 0.4f, c, light);
        if (p.key >= ImGuiKey_GamepadLStickLeft) {
            int dir = (p.key - ImGuiKey_GamepadLStickLeft) % 4;   // left, right, up, down
            float dx = dir == 0 ? -1.f : dir == 1 ? 1.f : 0.f, dy = dir == 2 ? -1.f : dir == 3 ? 1.f : 0.f;
            ImVec2 tip(c.x + dx * r * 1.1f, c.y + dy * r * 1.1f), base(c.x + dx * r * 0.7f, c.y + dy * r * 0.7f);
            ImVec2 side(-dy * r * 0.28f, dx * r * 0.28f);
            dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y), light);
        }
        return;
    }
    // Shoulders, triggers, the menu buttons: a pill with its name
    ImVec2 a(pos.x + gap, cy - r * 0.8f), b(pos.x + w - gap, cy + r * 0.8f);
    dl->AddRectFilled(a, b, dark, r * 0.8f);
    dl->AddRect(a, b, rim, r * 0.8f, 0, std::max(1.f, size * 0.04f));
    centred(shortLabel(p.type, p.key), size * 0.5f, ImVec2((a.x + b.x) * 0.5f, cy), light);
}

}  // namespace prompts
