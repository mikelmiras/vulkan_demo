// Volumetric clouds' shape (render/cloud_shape.h): flat bases, domes as tall as the cover is dense
#include "test.h"

#include <cmath>

#include "render/cloud_shape.h"

TEST(cloud_shape_domes) {
    CHECK(clouds::density(0.f, 0.3f) == 0.f);   // (no cover: no cloud)
    CHECK(clouds::density(1.f, 0.f) == 0.f && clouds::density(1.f, 1.f) == 0.f);   // (outside the slab)
    // Dense cover: cloud from just above the base most of the way up
    CHECK(clouds::density(1.f, 0.1f) > 0.9f);
    CHECK(clouds::density(1.f, 0.7f) > 0.5f);
    // Thin cover: a low, flat cloud, gone further up
    CHECK(clouds::density(0.45f, 0.1f) > 0.5f);
    CHECK(clouds::density(0.45f, 0.6f) == 0.f);
    // The base is flat: the same just above it whatever the cover's edge
    CHECK(clouds::density(0.6f, 0.05f) > 0.99f && clouds::density(0.9f, 0.05f) > 0.99f);
    // Taller where denser: the height where it ends grows with the cover
    auto topOf = [](float cover) { float h = 0.05f;   // (from above the base's ramp)
        while (h < 1.f && clouds::density(cover, h + 0.01f) > 0.5f) h += 0.01f; return h; };
    CHECK(topOf(0.5f) < topOf(0.8f) && topOf(0.8f) < topOf(1.f));
}

// The clouds' history found by direction: what's straight ahead now was off to the side before a turn; a camera that
// only moved sees it where it was; behind the last camera: none
TEST(cloud_history_by_direction) {
    const float kDeg = 3.14159265f / 180.f;
    Mat4 proj = perspective(60.f * kDeg, 16.f / 9.f, 0.1f, 2000.f, true);
    Vec3 ahead = normalize(Vec3{0.f, 0.3f, -1.f});
    float u = 0.f, v = 0.f, u2 = 0.f, v2 = 0.f;
    Mat4 still = proj * lookAt({0, 2, 0}, Vec3{0, 2, 0} + ahead, {0, 1, 0});
    REQUIRE(clouds::historyUv(still, ahead, u, v));
    CHECK_NEAR(u, 0.5f, 1e-4);
    CHECK_NEAR(v, 0.5f, 1e-4);
    Mat4 moved = proj * lookAt({300, 2, -40}, Vec3{300, 2, -40} + ahead, {0, 1, 0});   // (moved, not turned)
    REQUIRE(clouds::historyUv(moved, ahead, u2, v2));
    CHECK_NEAR(u2, u, 1e-4);
    CHECK_NEAR(v2, v, 1e-4);
    // Turned 10 degrees left last frame: what's ahead now was right of the middle then
    Vec3 left = normalize(Vec3{-std::sin(10.f * kDeg), 0.3f, -std::cos(10.f * kDeg)});
    Mat4 turned = proj * lookAt({0, 2, 0}, Vec3{0, 2, 0} + left, {0, 1, 0});
    REQUIRE(clouds::historyUv(turned, ahead, u, v));
    CHECK(u > 0.55f && u < 0.75f);
    // Above the middle (clip y points down: v below 0.5) when the last camera looked lower
    Mat4 lower = proj * lookAt({0, 2, 0}, Vec3{0, 2, 0} + Vec3{0, 0, -1}, {0, 1, 0});
    REQUIRE(clouds::historyUv(lower, ahead, u, v));
    CHECK(v < 0.5f);
    CHECK(!clouds::historyUv(still, Vec3{0, 0, 1}, u, v));   // (behind)
}

// The clouds' shadow: points along one ray towards the sun share a texel (a roof and the ground below it, in line
// with the sun); the plane's own points where they are; outside the square: none (the flat layer's then); thick cloud
// lets through little, never under a quarter
TEST(cloud_shadow_lookup) {
    Vec3 L = normalize(Vec3{0.5f, 0.6f, -0.2f});
    float u = 0.f, v = 0.f, u2 = 0.f, v2 = 0.f;
    REQUIRE(clouds::shadowUv(-4000.f, -4000.f, 8000.f, L, {100.f, 0.f, 50.f}, u, v));
    CHECK_NEAR(u, 4100.f / 8000.f, 1e-5);
    CHECK_NEAR(v, 4050.f / 8000.f, 1e-5);
    Vec3 up = Vec3{100.f, 0.f, 50.f} + L * 40.f;   // (40 m along the ray: a roof in line)
    REQUIRE(clouds::shadowUv(-4000.f, -4000.f, 8000.f, L, up, u2, v2));
    CHECK_NEAR(u2, u, 1e-5);
    CHECK_NEAR(v2, v, 1e-5);
    CHECK(!clouds::shadowUv(-4000.f, -4000.f, 8000.f, L, {4500.f, 0.f, 0.f}, u, v));
    CHECK(clouds::shadowThrough(0.f) == 1.f);
    CHECK(clouds::shadowThrough(100.f) > 0.75f && clouds::shadowThrough(100.f) < 0.85f);   // (a thin edge)
    CHECK(clouds::shadowThrough(400.f) > 0.35f && clouds::shadowThrough(400.f) < 0.45f);   // (a full cloud's height)
    CHECK(clouds::shadowThrough(5000.f) == 0.25f);
}

// The sky's light under the clouds: as bright as the harmonics say where the layer above is average, darker under a
// cloud, brighter under a gap, within bounds
TEST(cloud_ambient_overhead) {
    CHECK_NEAR(clouds::ambientUnder(0.6f, 0.6f), 1.0, 1e-6);
    CHECK(clouds::ambientUnder(0.3f, 0.6f) < 0.7f);
    CHECK(clouds::ambientUnder(1.0f, 0.6f) > 1.3f);
    CHECK(clouds::ambientUnder(0.f, 0.9f) == 0.4f);
    CHECK(clouds::ambientUnder(1.f, 0.f) == 1.8f);
}
