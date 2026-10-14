#include "game/camera_shake.h"

#include <algorithm>
#include <cmath>

namespace shake {

namespace {

// Smooth noise in -1..1: a few sines of unrelated frequencies (no two channels alike)
float wobble(float t, float seed) {
    return (std::sin(t * 23.f + seed * 7.1f) * 0.5f + std::sin(t * 37.f + seed * 3.3f) * 0.3f +
            std::sin(t * 61.f + seed * 11.7f) * 0.2f);
}

}  // namespace

Offset at(float trauma, float time) {
    Offset o;
    float t = std::clamp(trauma, 0.f, 1.f);
    float s = t * t;
    if (s <= 0.f) return o;
    o.yaw = kMaxAngle * s * wobble(time, 1.f);
    o.pitch = kMaxAngle * s * wobble(time, 2.f);
    o.roll = 0.5f * kMaxAngle * s * wobble(time, 3.f);
    o.move = Vec3{wobble(time, 4.f), wobble(time, 5.f), wobble(time, 6.f) * 0.5f} * (kMaxOffset * s);
    return o;
}

float falloff(float amount, float distance, float radius) {
    if (radius <= 3.f) return distance <= radius ? amount : 0.f;
    float k = std::clamp((distance - 3.f) / (radius - 3.f), 0.f, 1.f);
    return amount * (1.f - k * k * (3.f - 2.f * k));
}

}  // namespace shake
