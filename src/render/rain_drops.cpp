#include "render/rain_drops.h"

#include <algorithm>
#include <cmath>

namespace raindrops {

namespace {

float fract(float x) { return x - std::floor(x); }
// (GLSL's: fract(sin(dot(c, (127.1, 311.7))) * 43758.5453))
float hash(float cx, float cy) { return fract(std::sin(cx * 127.1f + cy * 311.7f) * 43758.5453f); }

// A dome's slope inside the ellipse d (|d| < 1: under it), leaning out from its middle, at `height`
bool dome(float dx, float dy, float height, Vec3& out) {
    const float l2 = dx * dx + dy * dy;
    if (l2 >= 1.f) return false;
    const float k = height / std::max(std::sqrt(1.f - l2), 0.2f);
    out = {dx * k, dy * k, 1.f};
    return true;
}

}  // namespace

Vec3 dropAt(float qx, float qy, float t, float rain, float steep) {
    Vec3 result{0, 0, 0};
    if (rain <= 0.f) return result;
    // Beads sitting where they fell: cells 6 cm across, a bead in some of them (more in heavier rain)
    const float cx = std::floor(qx / 0.06f), cy = std::floor(qy / 0.06f);
    if (hash(cx, cy) < rain * 0.6f) {
        const float ox = hash(cx + 17.f, cy + 17.f) * 0.6f + 0.2f, oy = hash(cx + 31.f, cy + 31.f) * 0.6f + 0.2f;
        const float r = 0.2f + 0.2f * hash(cx + 7.f, cy + 7.f);
        dome((qx / 0.06f - cx - ox) / r, (qy / 0.06f - cy - oy) / r, 0.5f, result);
    }
    // Running drops (steep surfaces): columns 4 cm wide, each's drops sliding down at its own pace, a trail above each
    if (steep > 0.f) {
        const float col = std::floor(qx / 0.04f);
        const float speed = 0.05f + 0.15f * hash(col, 3.f);
        const float y = qy / 0.15f + t * speed / 0.15f + hash(col, 9.f) * 10.f;   // (cells 15 cm tall, moving down)
        const float row = std::floor(y);
        if (hash(col, row) < rain * 0.35f * steep) {
            const float dx = (fract(qx / 0.04f) - 0.5f) / 0.35f, dy = (fract(y) - 0.3f) / 0.18f;
            Vec3 d;
            if (dome(dx, dy, 0.6f, d)) result = d;
            else if (dy > 0.f && std::fabs(dx) < 0.3f) result.z = std::max(result.z, 0.5f * (1.f - fract(y)));
        }
    }
    return result;
}

}  // namespace raindrops
