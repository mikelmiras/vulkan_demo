#include "render/cloud_shape.h"

#include <algorithm>

namespace clouds {

namespace {
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
}  // namespace

float density(float cover, float h) {
    if (h <= 0.f || h >= 1.f || cover <= 0.f) return 0.f;
    return smoothstep(0.f, 0.15f, cover - 0.2f - 0.8f * h * h) * smoothstep(0.f, 0.03f, h);
}

bool historyUv(const Mat4& p, Vec3 d, float& u, float& v) {
    const float x = p.m[0] * d.x + p.m[4] * d.y + p.m[8] * d.z, y = p.m[1] * d.x + p.m[5] * d.y + p.m[9] * d.z;
    const float w = p.m[3] * d.x + p.m[7] * d.y + p.m[11] * d.z;
    if (w <= 1e-6f) return false;
    u = x / w * 0.5f + 0.5f;
    v = y / w * 0.5f + 0.5f;
    return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
}

}  // namespace clouds
