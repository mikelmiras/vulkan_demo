#include "render/volumetric_fog.h"

#include <algorithm>
#include <cmath>

namespace fog {

float Range::slicesPerLog() const { return (float)kCellsZ / std::log(far / near); }

float sliceDepth(const Range& r, float slice) { return r.near * std::exp(slice / r.slicesPerLog()); }

float volumeZ(const Range& r, float depth) {
    float s = std::log(std::max(depth, 1e-3f) / r.near) * r.slicesPerLog();
    return std::clamp((s - 0.5f) / (float)kCellsZ, 0.f, 1.f);
}

float phase(Vec3 travel, Vec3 view, float g) {
    float c = -dot(travel, view);
    float d = 1.f + g * g - 2.f * g * c;
    return (1.f - g * g) / (d * std::sqrt(d));
}

std::vector<Gathered> integrate(const Range& r, float stretch, const std::vector<Cell>& column) {
    std::vector<Gathered> out(column.size());
    Vec3 gathered;
    float through = 1.f, from = 0.f;
    for (size_t z = 0; z < column.size(); ++z) {
        float to = sliceDepth(r, (float)(z + 1));
        float len = (to - from) * stretch;
        float density = std::max(column[z].density, 1e-7f);
        float t = std::exp(-density * len);
        gathered = gathered + column[z].scattered * (through * (1.f - t) / density);
        through *= t;
        out[z] = {gathered, through};
        from = to;
    }
    return out;
}

}  // namespace fog
