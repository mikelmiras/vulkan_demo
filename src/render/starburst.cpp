#include "render/starburst.h"

#include <algorithm>
#include <cmath>

namespace starburst {

int lines(int blades) { return blades % 2 == 0 ? blades / 2 : blades; }

float pack(int blades, float strength) { return (float)std::clamp(blades, 4, 9) + std::clamp(strength, 0.f, 1.f) * 0.99f; }

void unpack(float packed, int& blades, float& strength) {
    blades = (int)std::floor(packed);
    strength = (packed - (float)blades) / 0.99f;
}

}  // namespace starburst
