#include "render/camera_fx.h"

#include <algorithm>
#include <cmath>

namespace camerafx {

namespace {
// The 3 x 3 taking a direction (x, y, z) to clip space's (x, y, w): a view-projection's rows x, y and w, columns
// x, y, z (column-major Mat4: element (row r, column c) at m[c * 4 + r])
void directionPart(const Mat4& vp, float out[9]) {
    const int rows[3] = {0, 1, 3};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = vp.m[c * 4 + rows[r]];
}
bool invert3(const float a[9], float out[9]) {
    float c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (std::fabs(det) < 1e-12f) return false;
    float inv = 1.f / det;
    out[0] = c00 * inv, out[1] = (a[2] * a[7] - a[1] * a[8]) * inv, out[2] = (a[1] * a[5] - a[2] * a[4]) * inv;
    out[3] = c01 * inv, out[4] = (a[0] * a[8] - a[2] * a[6]) * inv, out[5] = (a[2] * a[3] - a[0] * a[5]) * inv;
    out[6] = c02 * inv, out[7] = (a[1] * a[6] - a[0] * a[7]) * inv, out[8] = (a[0] * a[4] - a[1] * a[3]) * inv;
    return true;
}
}  // namespace

void skyMotion(const Mat4& viewProjNow, const Mat4& viewProjBefore, float h[9]) {
    float now[9], before[9], back[9];
    directionPart(viewProjNow, now);
    directionPart(viewProjBefore, before);
    for (int k = 0; k < 9; ++k) h[k] = k % 4 == 0 ? 1.f : 0.f;
    if (!invert3(now, back)) return;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.f;
            for (int k = 0; k < 3; ++k) s += before[r * 3 + k] * back[k * 3 + c];
            h[r * 3 + c] = s;
        }
}

namespace {
float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
}  // namespace

float motionTapWeight(float distance, float here, float there, float front) {
    here = std::max(here, 0.5f);
    there = std::max(there, 0.5f);
    front = std::clamp(front, 0.f, 1.f);
    const float cone = std::clamp(1.f - distance / there, 0.f, 1.f), back = std::clamp(1.f - distance / here, 0.f, 1.f);
    const float both = (1.f - smoothstep(0.95f * there, 1.05f * there, distance)) * (1.f - smoothstep(0.95f * here, 1.05f * here, distance));
    return front * cone + (1.f - front) * back + 2.f * both;
}

float nearCoverage(float covered, int taps) { return taps > 0 ? std::clamp(covered / (float)taps, 0.f, 1.f) : 0.f; }

}  // namespace camerafx
