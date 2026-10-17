#include "render/taa_filter.h"

#include <algorithm>
#include <cmath>

namespace taa {

std::array<float, 4> catmullRom(float f) {
    return {f * (-0.5f + f * (1.f - 0.5f * f)), 1.f + f * f * (-2.5f + 1.5f * f), f * (0.5f + f * (2.f - 1.5f * f)),
            f * f * (-0.5f + 0.5f * f)};
}

float historyWeight(float base, float movedPixels, float mismatch) {
    const float t = std::clamp(movedPixels / 16.f, 0.f, 1.f), moved = t * t * (3.f - 2.f * t);   // (smoothstep)
    return base * (1.f - 0.2f * moved) * (1.f - 0.35f * std::clamp(mismatch, 0.f, 1.f));
}

void neighbourhoodBox(const std::array<Vec3, 5>& colours, Vec3& lo, Vec3& hi) {
    Vec3 mn = colours[0], mx = colours[0], m1{0, 0, 0}, m2{0, 0, 0};
    for (const Vec3& c : colours) {
        mn = {std::min(mn.x, c.x), std::min(mn.y, c.y), std::min(mn.z, c.z)};
        mx = {std::max(mx.x, c.x), std::max(mx.y, c.y), std::max(mx.z, c.z)};
        m1 = m1 + c;
        m2 = m2 + Vec3{c.x * c.x, c.y * c.y, c.z * c.z};
    }
    const Vec3 mean = m1 * 0.2f, sq = m2 * 0.2f;
    auto sigma = [](float s, float m) { return std::sqrt(std::max(s - m * m, 0.f)) * 1.25f; };
    const Vec3 sd{sigma(sq.x, mean.x), sigma(sq.y, mean.y), sigma(sq.z, mean.z)};
    lo = {std::max(mn.x, mean.x - sd.x), std::max(mn.y, mean.y - sd.y), std::max(mn.z, mean.z - sd.z)};
    hi = {std::min(mx.x, mean.x + sd.x), std::min(mx.y, mean.y + sd.y), std::min(mx.z, mean.z + sd.z)};
}

float casSharpen(float centre, float up, float left, float right, float down, float amount) {
    const float mn = std::min({centre, up, left, right, down}), mx = std::max({centre, up, left, right, down});
    const float room = std::sqrt(std::clamp(std::min(mn, 1.f - mx) / std::max(mx, 1e-4f), 0.f, 1.f));
    const float w = room * (-1.f / (8.f + (5.f - 8.f) * std::clamp(amount, 0.f, 1.f)));
    return std::clamp(std::clamp((up + left + right + down) * w + centre, 0.f, 1.f) / (1.f + 4.f * w), 0.f, 0.999f);
}

}  // namespace taa

