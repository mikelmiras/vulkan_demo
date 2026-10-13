// render/shadow_cascades: the sun's cascades follow the camera they're made for (split screen's views each get
// their own), slices grow with distance, and their cameras snap to whole texels
#include "test.h"

#include <cmath>

#include "render/shadow_cascades.h"

namespace {

// Is `p` inside the cascade's box (clip space of an orthographic camera: no divide)?
bool inside(const Mat4& vp, Vec3 p) {
    Vec3 c = transformPoint(vp, p);
    return std::fabs(c.x) <= 1.f && std::fabs(c.y) <= 1.f && c.z >= 0.f && c.z <= 1.f;
}

}  // namespace

TEST(sun_cascades_follow_their_camera) {
    const Vec3 sun = normalize(Vec3{-0.45f, -1.f, -0.3f});
    shadows::Camera a{{0, 2, 0}, {0, 0, -1}, 1.f, 16.f / 9.f}, b{{600, 2, 300}, {1, 0, 0}, 1.f, 16.f / 9.f};
    Mat4 va[4], vb[4];
    float sa[4], sb[4];
    shadows::sunCascades(a, sun, 160.f, 2048.f, va, sa);
    shadows::sunCascades(b, sun, 160.f, 1024.f, vb, sb);
    for (int c = 1; c < 4; ++c) CHECK(sa[c] > sa[c - 1]);   // further slices further out...
    CHECK_NEAR(sa[3], 160.f, 1e-3f);                         // ... the last ending at the distance
    CHECK(sa[0] < 10.f);                                     // (the first one small: sharp near the camera)
    // What's just in front of each camera is in its own first cascade, not the other's
    Vec3 nearA = a.position + a.forward * 2.f, nearB = b.position + b.forward * 2.f;
    CHECK(inside(va[0], nearA));
    CHECK(inside(vb[0], nearB));
    for (int c = 0; c < 4; ++c) {
        CHECK(!inside(va[c], nearB));
        CHECK(!inside(vb[c], nearA));
    }
    // Something 60 m ahead: in a later cascade
    CHECK(inside(va[3], a.position + a.forward * 60.f) || inside(va[2], a.position + a.forward * 60.f));
    // Splits without cameras (no shadows): the same splits
    float only[4];
    shadows::sunCascades(a, sun, 160.f, 2048.f, nullptr, only);
    for (int c = 0; c < 4; ++c) CHECK(only[c] == sa[c]);
}

TEST(sun_cascades_snap_to_texels) {
    const Vec3 sun = normalize(Vec3{-0.45f, -1.f, -0.3f});
    // The world's origin lands on a whole texel of each map, wherever the camera is: moving doesn't shimmer
    for (float x : {0.f, 0.37f, 12.9f}) {
        shadows::Camera cam{{x, 3, x * 0.5f}, {0.2f, -0.1f, -1}, 1.f, 1.5f};
        Mat4 vp[4];
        float splits[4];
        shadows::sunCascades(cam, sun, 160.f, 1024.f, vp, splits);
        for (int c = 0; c < 4; ++c) {
            float ox = vp[c].m[12] * 512.f, oy = vp[c].m[13] * 512.f;
            CHECK_NEAR(ox, std::round(ox), 1e-2f);
            CHECK_NEAR(oy, std::round(oy), 1e-2f);
        }
    }
}
