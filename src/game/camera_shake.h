// Camera shake: "trauma" (0..1) added by hits, explosions and footfalls, decaying over time; the camera turns and
// moves by smooth noise scaled by trauma squared (small knocks barely show, big ones shake hard). Deterministic in
// game time, so a replay or a paused game shows the same.
#pragma once

#include "core/vecmath.h"

namespace shake {

constexpr float kDecay = 1.6f;              // trauma lost per second
constexpr float kMaxAngle = 4.f * DEG2RAD;  // at full trauma: yaw and pitch (roll half of it)
constexpr float kMaxOffset = 0.06f;         // ... and metres of travel

struct Offset {
    float yaw = 0.f, pitch = 0.f, roll = 0.f;   // radians
    Vec3 move;                                  // metres, in the camera's own axes
};

// The shake at game time `time` for `trauma`
Offset at(float trauma, float time);
// How much of a shake of `amount` at `distance` metres from the camera reaches it: all within 3 m, none past
// `radius`, smoothly between
float falloff(float amount, float distance, float radius);

}  // namespace shake
