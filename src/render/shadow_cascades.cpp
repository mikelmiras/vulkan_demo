#include "render/shadow_cascades.h"

#include <algorithm>
#include <cmath>

namespace shadows {

void sunCascades(const Camera& camera, Vec3 sunDir, float distance, float texels, Mat4* viewProj, float* splits,
                 float (*extent)[2]) {
    sunCascades(camera, sunDir, 0.f, distance, texels, viewProj, splits, extent);
}

void sunCascades(const Camera& camera, Vec3 sunDir, float nearest, float distance, float texels, Mat4* viewProj,
                 float* splits, float (*extent)[2]) {
    Vec3 fwd = normalize(camera.forward);
    Vec3 right = std::fabs(fwd.y) > 0.999f ? Vec3{1, 0, 0} : normalize(cross(fwd, {0, 1, 0}));
    Vec3 up = cross(right, fwd);
    float tanY = std::tan(camera.fovY * 0.5f), tanX = tanY * std::max(camera.aspect, 0.01f);
    const float n = std::max(nearest, 0.1f), f = std::max(nearest, 0.f) + std::max(distance, 1.f);
    float prev = n;
    for (int c = 0; c < kCascades; ++c) {
        float t = (float)(c + 1) / (float)kCascades;
        float split = 0.8f * n * std::pow(f / n, t) + 0.2f * (n + (f - n) * t);   // mostly logarithmic
        splits[c] = split;
        if (viewProj) {
            // A sphere around the slice (rotating the view doesn't change its size), then an orthographic camera
            // looking down the sun around it, with room behind for casters between the sun and the slice
            Vec3 center = camera.position + fwd * ((prev + split) * 0.5f);
            float r = 0.f;
            for (float d : {prev, split})
                for (float sx : {-1.f, 1.f})
                    for (float sy : {-1.f, 1.f}) {
                        Vec3 corner = camera.position + fwd * d + right * (sx * d * tanX) + up * (sy * d * tanY);
                        r = std::max(r, length(corner - center));
                    }
            r = std::ceil(r * 16.f) / 16.f;
            const float behind = kBehind;
            if (extent) extent[c][0] = 2.f * r, extent[c][1] = 2.f * r + behind;
            Vec3 sunUp = std::fabs(sunDir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            Mat4 view = lookAt(center - sunDir * (r + behind), center, sunUp);
            Mat4 proj = orthographic(-r, r, -r, r, 0.1f, 2.f * r + behind);
            Mat4 vp = proj * view;
            float half = texels * 0.5f;
            float ox = vp.m[12] * half, oy = vp.m[13] * half;   // where the world origin lands, in texels
            proj.m[12] += (std::round(ox) - ox) / half;
            proj.m[13] += (std::round(oy) - oy) / half;
            viewProj[c] = proj * view;
        }
        prev = split;
    }
}

}  // namespace shadows
