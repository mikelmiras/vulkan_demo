#include "render/light_shafts.h"

#include <algorithm>

namespace shafts {

Params choose(float strength, bool volumetricFog, int quality, const std::vector<bool>& spots) {
    Params p;
    if (strength <= 0.f || volumetricFog || quality <= 0) return p;
    p.density = std::min(strength, 2.f) * 0.004f;   // (1: a light haze, the sun's rays clear in it)
    p.steps = quality >= 2 ? 24 : 16;
    int n = 0;
    for (size_t i = 0; i < spots.size() && n < 2; ++i)
        if (spots[i]) p.lamps[n++] = (int)i;
    return p;
}

}  // namespace shafts
