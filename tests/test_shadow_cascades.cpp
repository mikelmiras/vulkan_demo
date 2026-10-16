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

// Fitted to what's in view: an overhead camera 100 m up sees nothing nearer than about 100 m, and its cascades all
// fall between there and the shadow distance beyond, instead of mostly on empty air in front of it
TEST(shadow_cascades_fitted_to_the_view) {
    shadows::Camera cam{{0, 100, 0}, normalize(Vec3{0, -1, 0.3f}), 0.35f, 16.f / 9.f};
    Vec3 sun = normalize(Vec3{-0.4f, -1.f, -0.3f});
    float near[4], fitted[4];
    shadows::sunCascades(cam, sun, 60.f, 1024.f, nullptr, near);
    shadows::sunCascades(cam, sun, 95.f, 60.f, 1024.f, nullptr, fitted);
    CHECK(near[0] < 5.f);                          // (from the camera: the first slice ends in mid-air)
    CHECK(fitted[0] > 95.f && fitted[0] < 110.f);  // (fitted: every slice on the ground)
    CHECK_NEAR(fitted[3], 155.0, 1e-3);
    for (int c = 1; c < 4; ++c) CHECK(fitted[c] > fitted[c - 1]);
    float extentNear[4][2], extentFitted[4][2];
    Mat4 vp[4];
    shadows::sunCascades(cam, sun, 60.f, 1024.f, vp, near, extentNear);
    shadows::sunCascades(cam, sun, 95.f, 60.f, 1024.f, vp, fitted, extentFitted);
    CHECK(extentFitted[3][0] > extentNear[0][0]);   // (well: the slices are where they're needed, wider for it)
}

TEST(shadow_casters_culled_by_the_view) {
    // A camera at the origin looking down -z (60 degrees up and down, 16:9); the view from 0.1 to 100 m
    shadows::Camera cam{{0, 0, 0}, {0, 0, -1}, 60.f * DEG2RAD, 16.f / 9.f};
    auto casts = [&](Vec3 sun, float floor, Vec3 c, float r = 0.5f, float widen = 0.05f) {
        return shadows::castsIntoView(cam, 0.1f, 100.f, widen, normalize(sun), floor, c, r);
    };
    const Vec3 overhead{0, -1, 0};
    CHECK(casts(overhead, 0.f, {0, 1, -10}));          // in front: its shadow under it, in view
    CHECK(!casts(overhead, 0.f, {0, 1, 10}));          // behind the camera: its shadow too
    CHECK(!casts(overhead, 0.f, {200, 1, -10}));       // far off to the side
    CHECK(!casts(overhead, 0.f, {0, 1, -300}));        // past the view's far end
    CHECK(casts(overhead, 0.f, {0, 40, -10}));         // high above the view (out of it): its shadow falls into it
    // A low sun shining away from the camera: a tall thing behind it throws its shadow forward into view, unless
    // the lowest thing in view is so high the shadow has passed under it by then
    const Vec3 low{0, -0.2f, -1};
    CHECK(casts(low, 0.f, {0, 5, 10}));
    CHECK(!casts(low, 4.f, {0, 5, 10}));
    // Just outside the side of the view (50 degrees off, the edge at 46): out, unless widened for a far cascade
    Vec3 side{20.f * std::tan(50.f * DEG2RAD), 1.f, -20.f};
    CHECK(!casts(overhead, 0.f, side));
    CHECK(casts(overhead, 0.f, side, 0.5f, 0.35f));
    // A big caster reaching into view from outside it
    CHECK(casts(overhead, 0.f, side, 6.f));
}
