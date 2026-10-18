#include "render/area_light.h"

#include <algorithm>
#include <cmath>

namespace arealight {

Vec3 nearestOnSegment(Vec3 a, Vec3 b, Vec3 p) {
    const Vec3 d = b - a;
    const float dd = dot(d, d);
    const float t = dd > 1e-12f ? std::clamp(dot(p - a, d) / dd, 0.f, 1.f) : 0.f;
    return a + d * t;
}

Vec3 segmentNearestRay(Vec3 l0, Vec3 l1, Vec3 r) {
    const Vec3 ld = l1 - l0;
    const float rl0 = dot(r, l0), rld = dot(r, ld), l0ld = dot(l0, ld), denom = dot(ld, ld) - rld * rld;
    const float t = denom > 1e-8f ? std::clamp((rl0 * rld - l0ld) / denom, 0.f, 1.f) : 0.5f;
    return l0 + ld * t;
}

Vec3 sphereNearestRay(Vec3 c, Vec3 r, float radius) {
    const Vec3 toRay = r * dot(c, r) - c;
    const float len = length(toRay);
    return len > 1e-6f ? c + toRay * std::min(radius / len, 1.f) : c;
}

float highlightScale(float alpha, float radius, float length, float distance) {
    alpha = std::max(alpha, 1e-4f);
    distance = std::max(distance, 1e-4f);
    const float sphere = alpha / std::min(alpha + radius / (2.f * distance), 1.f);
    const float tube = alpha / std::min(alpha + length / (2.f * distance), 1.f);
    return sphere * sphere * tube;
}

Vec3 rectNearest(Vec3 c, Vec3 x, Vec3 y, float hx, float hy) {
    const Vec3 d = c * -1.f;   // (the shaded point, from the centre)
    return c + x * std::clamp(dot(d, x), -hx, hx) + y * std::clamp(dot(d, y), -hy, hy);
}

Vec3 rectRayPoint(Vec3 c, Vec3 x, Vec3 y, float hx, float hy, Vec3 r) {
    const Vec3 n = cross(x, y);
    const float along = dot(r, n), t = std::abs(along) > 1e-6f ? dot(c, n) / along : -1.f;
    const Vec3 hit = t > 0.f ? r * t : r * (length(c) * 4.f);   // (away or parallel: far along the ray)
    const Vec3 d = hit - c;
    return c + x * std::clamp(dot(d, x), -hx, hx) + y * std::clamp(dot(d, y), -hy, hy);
}

void pictureUv(Vec3 rel, Vec3 x, Vec3 y, float w, float h, float& u, float& v) {
    u = 0.5f - dot(rel, x) / w;
    v = 0.5f - dot(rel, y) / h;
}

float pictureLod(float distance, float size) {
    return std::log2(std::max(256.f * std::clamp(distance / size, 0.f, 1.f), 1.f));
}

float reach(float range, float length, float height) {
    if (length <= 0.f) return range;
    if (height <= 0.f) return range + 0.5f * length;
    return range + 0.5f * std::sqrt(length * length + height * height);
}

}  // namespace arealight
