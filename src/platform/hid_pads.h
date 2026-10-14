// Gamepads read as raw HID reports (Windows: XInput only knows Xbox pads, so Sony's and Nintendo's are read here):
// which pad a USB vendor / product id is, its input reports into a GamepadState, and the output reports for rumble,
// light bars and the Switch Pro Controller's start-up. Plain functions on bytes (tested without the pads); the
// platform layer opens the devices and moves the reports.
//
// Report layouts, after the Linux drivers (hid-playstation, hid-nintendo) and dekuNukem's notes on the Switch:
//   - DualShock 4: 0x01 (USB, and Bluetooth's short report), 0x11 (Bluetooth, two bytes further on)
//   - DualSense: 0x01 (USB: 64 bytes; Bluetooth's short report has the DualShock 4's layout), 0x31 (Bluetooth)
//   - Switch Pro Controller: 0x30 (full, once asked: switchStartup()), 0x3F (Bluetooth's simple report)
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "scene/input_bindings.h"

namespace hidpad {

enum class Model { None, DualShock4, DualSense, SwitchPro };

Model modelOf(uint16_t vendor, uint16_t product);
const char* modelName(Model model);   // "DualSense", ...
PadType typeOf(Model model);

// An input report (the report id first) into `state` (connected, its type and name set too); false if it carries
// no controls (other reports: replies, battery)
bool parseReport(Model model, const uint8_t* report, size_t size, GamepadState& state);

// The output report setting rumble (the heavy and light motors, 0..1) and the light bar (0..1), USB layout;
// empty for pads without (the Switch Pro Controller's rumble is encoded otherwise: not done)
std::vector<uint8_t> outputReport(Model model, float low, float high, const float light[3]);

// What to send a Switch Pro Controller so it sends full reports (0x30) and lights its first player light, in
// order: over USB the handshake first. Each padded to `size` (the device's output report length).
std::vector<std::vector<uint8_t>> switchStartup(bool usb, size_t size);

}  // namespace hidpad
