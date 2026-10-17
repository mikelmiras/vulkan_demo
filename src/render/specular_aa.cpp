#include "render/specular_aa.h"

#include <algorithm>
#include <cmath>

namespace specaa {

float normalMapVariance(float filteredLength, float strength) {
    const float len = std::clamp(filteredLength, 1e-3f, 1.f);
    return (1.f - len) / len * strength * strength;
}

float widenedRoughness(float roughness, float screen, float texture) {
    const float a = roughness * roughness;
    const float a2 = a * a + std::min(2.f * std::max(screen, 0.f), 0.18f) + std::min(std::max(texture, 0.f), 0.18f);
    return std::sqrt(std::sqrt(std::clamp(a2, 0.f, 1.f)));
}

}  // namespace specaa
