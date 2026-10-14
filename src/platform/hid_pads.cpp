#include "platform/hid_pads.h"

#include <algorithm>
#include <cmath>

namespace hidpad {

namespace {

constexpr uint16_t kSony = 0x054C, kNintendo = 0x057E;

// A stick's axis (-1..1, + right / down) into its two gamepad keys, with a dead zone
void axis(GamepadState& s, ImGuiKey negative, ImGuiKey positive, float v) {
    const float dead = 0.12f;
    v = std::clamp(v, -1.f, 1.f);
    s.set(negative, std::clamp((-v - dead) / (1.f - dead), 0.f, 1.f));
    s.set(positive, std::clamp((v - dead) / (1.f - dead), 0.f, 1.f));
}

// Sony's sticks: a byte each, 128 in the middle, y down
void sonySticks(GamepadState& s, const uint8_t* p) {
    auto f = [](uint8_t b) { return ((float)b - 128.f) / 127.f; };
    axis(s, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, f(p[0]));
    axis(s, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, f(p[1]));
    axis(s, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, f(p[2]));
    axis(s, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, f(p[3]));
}

// A hat: 0 up, then clockwise in eighths; anything else (8) centred
void hat(GamepadState& s, int h) {
    bool up = h == 7 || h == 0 || h == 1, right = h >= 1 && h <= 3, down = h >= 3 && h <= 5, left = h >= 5 && h <= 7;
    s.set(ImGuiKey_GamepadDpadUp, up ? 1.f : 0.f);
    s.set(ImGuiKey_GamepadDpadRight, right ? 1.f : 0.f);
    s.set(ImGuiKey_GamepadDpadDown, down ? 1.f : 0.f);
    s.set(ImGuiKey_GamepadDpadLeft, left ? 1.f : 0.f);
}

void button(GamepadState& s, ImGuiKey key, uint8_t bits, uint8_t mask) { s.set(key, (bits & mask) ? 1.f : 0.f); }

// Both Sony pads' buttons: the hat and the face buttons in one byte, shoulders and the rest in the next
void sonyButtons(GamepadState& s, uint8_t b0, uint8_t b1) {
    hat(s, b0 & 0x0F);
    button(s, ImGuiKey_GamepadFaceLeft, b0, 0x10);    // square
    button(s, ImGuiKey_GamepadFaceDown, b0, 0x20);    // cross
    button(s, ImGuiKey_GamepadFaceRight, b0, 0x40);   // circle
    button(s, ImGuiKey_GamepadFaceUp, b0, 0x80);      // triangle
    button(s, ImGuiKey_GamepadL1, b1, 0x01);
    button(s, ImGuiKey_GamepadR1, b1, 0x02);
    button(s, ImGuiKey_GamepadBack, b1, 0x10);        // share / create
    button(s, ImGuiKey_GamepadStart, b1, 0x20);       // options
    button(s, ImGuiKey_GamepadL3, b1, 0x40);
    button(s, ImGuiKey_GamepadR3, b1, 0x80);
}

// The DualShock 4's layout (and the DualSense's short Bluetooth report): sticks, buttons (3 bytes), triggers
bool ds4Layout(GamepadState& s, const uint8_t* p, size_t n) {
    if (n < 9) return false;
    sonySticks(s, p);
    sonyButtons(s, p[4], p[5]);
    s.set(ImGuiKey_GamepadL2, p[7] / 255.f);
    s.set(ImGuiKey_GamepadR2, p[8] / 255.f);
    return true;
}

// The DualSense's full report: sticks, triggers, a counter, then the buttons
bool dualSenseLayout(GamepadState& s, const uint8_t* p, size_t n) {
    if (n < 10) return false;
    sonySticks(s, p);
    s.set(ImGuiKey_GamepadL2, p[4] / 255.f);
    s.set(ImGuiKey_GamepadR2, p[5] / 255.f);
    sonyButtons(s, p[7], p[8]);
    return true;
}

// The Switch Pro Controller's full report (0x30): buttons in three bytes, sticks as 12-bit pairs (y up)
bool switchFull(GamepadState& s, const uint8_t* p, size_t n) {
    if (n < 12) return false;
    uint8_t right = p[3], shared = p[4], left = p[5];
    button(s, ImGuiKey_GamepadFaceLeft, right, 0x01);    // Y
    button(s, ImGuiKey_GamepadFaceUp, right, 0x02);      // X
    button(s, ImGuiKey_GamepadFaceDown, right, 0x04);    // B
    button(s, ImGuiKey_GamepadFaceRight, right, 0x08);   // A
    button(s, ImGuiKey_GamepadR1, right, 0x40);
    s.set(ImGuiKey_GamepadR2, (right & 0x80) ? 1.f : 0.f);   // (ZR: digital)
    button(s, ImGuiKey_GamepadBack, shared, 0x01);       // minus
    button(s, ImGuiKey_GamepadStart, shared, 0x02);      // plus
    button(s, ImGuiKey_GamepadR3, shared, 0x04);
    button(s, ImGuiKey_GamepadL3, shared, 0x08);
    button(s, ImGuiKey_GamepadDpadDown, left, 0x01);
    button(s, ImGuiKey_GamepadDpadUp, left, 0x02);
    button(s, ImGuiKey_GamepadDpadRight, left, 0x04);
    button(s, ImGuiKey_GamepadDpadLeft, left, 0x08);
    button(s, ImGuiKey_GamepadL1, left, 0x40);
    s.set(ImGuiKey_GamepadL2, (left & 0x80) ? 1.f : 0.f);
    auto stick = [&](const uint8_t* d, ImGuiKey l, ImGuiKey r, ImGuiKey u, ImGuiKey dn) {
        int x = d[0] | (d[1] & 0x0F) << 8, y = d[1] >> 4 | d[2] << 4;
        const float range = 1600.f;   // (uncalibrated: about what a stick's travel is either side of 2048)
        axis(s, l, r, (x - 2048) / range);
        axis(s, u, dn, -(y - 2048) / range);
    };
    stick(p + 6, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown);
    stick(p + 9, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown);
    return true;
}

// Its simple Bluetooth report (0x3F): buttons in two bytes, a hat, sticks as 16-bit values (y down)
bool switchSimple(GamepadState& s, const uint8_t* p, size_t n) {
    if (n < 12) return false;
    button(s, ImGuiKey_GamepadFaceDown, p[1], 0x01);    // B
    button(s, ImGuiKey_GamepadFaceRight, p[1], 0x02);   // A
    button(s, ImGuiKey_GamepadFaceLeft, p[1], 0x04);    // Y
    button(s, ImGuiKey_GamepadFaceUp, p[1], 0x08);      // X
    button(s, ImGuiKey_GamepadL1, p[1], 0x10);
    button(s, ImGuiKey_GamepadR1, p[1], 0x20);
    s.set(ImGuiKey_GamepadL2, (p[1] & 0x40) ? 1.f : 0.f);
    s.set(ImGuiKey_GamepadR2, (p[1] & 0x80) ? 1.f : 0.f);
    button(s, ImGuiKey_GamepadBack, p[2], 0x01);
    button(s, ImGuiKey_GamepadStart, p[2], 0x02);
    button(s, ImGuiKey_GamepadL3, p[2], 0x04);
    button(s, ImGuiKey_GamepadR3, p[2], 0x08);
    hat(s, p[3] & 0x0F);
    auto f = [&](int i) { return ((float)(p[i] | p[i + 1] << 8) - 32768.f) / 32767.f; };
    axis(s, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, f(4));
    axis(s, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, f(6));
    axis(s, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, f(8));
    axis(s, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, f(10));
    return true;
}

}  // namespace

Model modelOf(uint16_t vendor, uint16_t product) {
    if (vendor == kSony) {
        if (product == 0x05C4 || product == 0x09CC || product == 0x0BA0) return Model::DualShock4;   // (and its USB dongle)
        if (product == 0x0CE6 || product == 0x0DF2) return Model::DualSense;                       // (and the Edge)
    }
    if (vendor == kNintendo && product == 0x2009) return Model::SwitchPro;
    return Model::None;
}

const char* modelName(Model model) {
    switch (model) {
    case Model::DualShock4: return "DualShock 4";
    case Model::DualSense: return "DualSense";
    case Model::SwitchPro: return "Switch Pro Controller";
    default: return "HID pad";
    }
}

PadType typeOf(Model model) {
    if (model == Model::SwitchPro) return PadType::Switch;
    if (model == Model::None) return PadType::Generic;
    return PadType::PlayStation;
}

bool parseReport(Model model, const uint8_t* r, size_t n, GamepadState& s) {
    if (n < 1) return false;
    bool ok = false;
    switch (model) {
    case Model::DualShock4:
        if (r[0] == 0x01) ok = ds4Layout(s, r + 1, n - 1);
        else if (r[0] == 0x11 && n > 3) ok = ds4Layout(s, r + 3, n - 3);
        break;
    case Model::DualSense:
        if (r[0] == 0x01) ok = n >= 64 ? dualSenseLayout(s, r + 1, n - 1) : ds4Layout(s, r + 1, n - 1);
        else if (r[0] == 0x31 && n > 2) ok = dualSenseLayout(s, r + 2, n - 2);
        break;
    case Model::SwitchPro:
        if (r[0] == 0x30) ok = switchFull(s, r, n);
        else if (r[0] == 0x3F) ok = switchSimple(s, r, n);
        break;
    default: break;
    }
    if (ok) {
        s.connected = true;
        s.type = typeOf(model);
        s.name = modelName(model);
    }
    return ok;
}

std::vector<uint8_t> outputReport(Model model, float low, float high, const float light[3]) {
    auto byte = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    std::vector<uint8_t> r;
    if (model == Model::DualShock4) {
        r.assign(32, 0);
        r[0] = 0x05;
        r[1] = 0x07;   // rumble, light bar, flashing (none)
        r[2] = 0x04;
        r[4] = byte(high);   // (the light motor)
        r[5] = byte(low);
        r[6] = byte(light[0]);
        r[7] = byte(light[1]);
        r[8] = byte(light[2]);
    } else if (model == Model::DualSense) {
        r.assign(48, 0);
        r[0] = 0x02;
        r[1] = 0x03;   // compatible vibration (the motors, not haptics), haptics select
        r[2] = 0x04;   // light bar control
        r[3] = byte(high);
        r[4] = byte(low);
        r[39] = 0x02;  // light bar setup control
        r[42] = 0x02;  // ... light out: the light bar is ours, not the start-up animation's
        r[45] = byte(light[0]);
        r[46] = byte(light[1]);
        r[47] = byte(light[2]);
    }
    return r;
}

std::vector<std::vector<uint8_t>> switchStartup(bool usb, size_t size) {
    std::vector<std::vector<uint8_t>> out;
    auto padded = [&](std::vector<uint8_t> r) {
        if (r.size() < size) r.resize(size, 0);
        out.push_back(std::move(r));
    };
    if (usb) {   // handshake, 3 Mbit, handshake again, then HID only (no USB timeout)
        padded({0x80, 0x02});
        padded({0x80, 0x03});
        padded({0x80, 0x02});
        padded({0x80, 0x04});
    }
    // Subcommands (report 0x01): a packet number, neutral rumble, the subcommand and its argument
    uint8_t packet = 0;
    auto sub = [&](uint8_t id, uint8_t arg) {
        padded({0x01, (uint8_t)(packet++ & 0x0F), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, id, arg});
    };
    sub(0x03, 0x30);   // input report mode: full (0x30)
    sub(0x30, 0x01);   // player lights: the first
    return out;
}

}  // namespace hidpad
