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

}  // namespace clouds
