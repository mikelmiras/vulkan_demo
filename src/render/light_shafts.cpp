#include "render/light_shafts.h"

#include <algorithm>
#include <cmath>

namespace shafts {

Params choose(float strength, bool volumetricFog, int quality, const std::vector<float>& lamps) {
    Params p;
    if (strength <= 0.f || volumetricFog || quality <= 0) return p;
    p.density = std::min(strength, 2.f) * 0.004f;   // (1: a light haze, the sun's rays clear in it)
    p.steps = quality >= 2 ? 24 : 16;
    std::vector<int> order;
    for (size_t i = 0; i < lamps.size(); ++i)
        if (lamps[i] > 0.f) order.push_back((int)i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return lamps[a] > lamps[b]; });
    order.resize(std::min<size_t>(order.size(), kMaxLamps));
    std::sort(order.begin(), order.end());
    for (int i : order) p.lamps[p.lampCount++] = i;
    return p;
}

float prominence(float brightness, float range, float distance) {
    if (brightness <= 0.f || range <= 0.f) return 0.f;
    return brightness * range * range / (range * range + distance * distance);
}

float halo(float tc, float h, float t0, float t1, float soft) {
    if (t1 <= t0) return 0.f;
    const float e = std::sqrt(h * h + soft * soft);   // (atan's antiderivative of 1 / (e^2 + s^2))
    return (std::atan((t1 - tc) / e) - std::atan((t0 - tc) / e)) / e;
}

}  // namespace shafts
