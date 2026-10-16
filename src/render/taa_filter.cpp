#include "render/taa_filter.h"

#include <algorithm>

namespace taa {

std::array<float, 4> catmullRom(float f) {
    return {f * (-0.5f + f * (1.f - 0.5f * f)), 1.f + f * f * (-2.5f + 1.5f * f), f * (0.5f + f * (2.f - 1.5f * f)),
            f * f * (-0.5f + 0.5f * f)};
}

float historyWeight(float base, float movedPixels, float mismatch) {
    const float t = std::clamp(movedPixels / 16.f, 0.f, 1.f), moved = t * t * (3.f - 2.f * t);   // (smoothstep)
    return base * (1.f - 0.2f * moved) * (1.f - 0.35f * std::clamp(mismatch, 0.f, 1.f));
}

}  // namespace taa
