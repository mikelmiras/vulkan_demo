#include "render/gi_denoise.h"

#include <algorithm>
#include <cmath>

namespace gidenoise {

float tapWeight(int x, int y, float z, float tapZ, float normalDot) {
    float w = (x == 0 ? 1.f : 0.5f) * (y == 0 ? 1.f : 0.5f);
    w *= std::exp(-std::abs(tapZ - z) / (0.03f * z + 0.02f));   // (the same surface's depth, about)
    w *= std::pow(std::max(normalDot, 0.f), 8.f);               // (facing the same way)
    return w;
}

}  // namespace gidenoise
