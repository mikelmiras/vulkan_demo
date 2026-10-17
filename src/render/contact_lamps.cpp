#include "render/contact_lamps.h"

#include <algorithm>
#include <cmath>

namespace contactlamps {

std::array<int, kMax> choose(const std::vector<bool>& shadowed) {
    std::array<int, kMax> out;
    out.fill(-1);
    int n = 0;
    for (size_t i = 0; i < shadowed.size() && n < kMax; ++i)
        if (shadowed[i]) out[(size_t)n++] = (int)i;
    return out;
}

float pack3(float a, float b, float c) {
    auto q = [](float v) { return std::round(std::clamp(v, 0.f, 1.f) * 255.f); };
    return q(a) + q(b) * 256.f + q(c) * 65536.f;
}

void unpack3(float packed, float& a, float& b, float& c) {
    const float p = std::round(packed);   // (not floor(p + 0.5): at 2^24 - 1 the half rounds it up to 2^24)
    a = std::fmod(p, 256.f) / 255.f;
    b = std::fmod(std::floor(p / 256.f), 256.f) / 255.f;
    c = std::floor(p / 65536.f) / 255.f;
}

}  // namespace contactlamps
