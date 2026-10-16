#include "render/light_shafts.h"

#include <algorithm>
#include <cmath>

namespace shafts {

SunOnScreen sunOnScreen(const Mat4& viewProj, Vec3 camPos, Vec3 sunDir, float vx, float vy, float vw, float vh) {
    SunOnScreen s;
    // A point far towards the sun, into clip space (Vulkan: y down the screen)
    Vec3 p = camPos - normalize(sunDir) * 10000.f;
    const float* m = viewProj.m;
    float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (cw <= 1e-4f) return s;   // (behind the camera)
    float nx = cx / cw, ny = cy / cw;
    s.x = vx + (nx * 0.5f + 0.5f) * vw;
    s.y = vy + (ny * 0.5f + 0.5f) * vh;
    // How far outside the viewport, in its sizes (0 inside)
    float ox = std::max(std::fabs(nx) - 1.f, 0.f) * 0.5f, oy = std::max(std::fabs(ny) - 1.f, 0.f) * 0.5f;
    float out = std::max(ox, oy);
    s.fade = std::clamp(1.f - out * 3.f, 0.f, 1.f);
    return s;
}

}  // namespace shafts
