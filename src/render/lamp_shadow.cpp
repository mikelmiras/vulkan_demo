#include "render/lamp_shadow.h"

#include <algorithm>

namespace lampshadow {

float blockerFraction(float d, float clipZ, float clipW, float lightZ) {
    const float denom = d * clipW - clipZ + lightZ;
    return std::abs(denom) > 1e-12f ? lightZ / denom : 1.f;
}

float penumbra(float size, float t) {
    t = std::clamp(t, 0.02f, 1.f);
    return size * (1.f - t) / t;
}

}  // namespace lampshadow
