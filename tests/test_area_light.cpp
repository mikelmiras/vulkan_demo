// Lights with a shape (render/area_light.h): the tube's and the sphere's representative points, the highlight's scale
#include "test.h"

#include "render/area_light.h"

TEST(area_light_nearest_points) {
    // A tube from x = -1 to 1 two metres up: the point over x = 0.5 is the nearest; past its end, the end
    CHECK_VEC(arealight::nearestOnSegment({-1, 2, 0}, {1, 2, 0}, {0.5f, 0, 0}), test::v3(0.5f, 2.f, 0.f), 1e-6f);
    CHECK_VEC(arealight::nearestOnSegment({-1, 2, 0}, {1, 2, 0}, {5, 0, 0}), test::v3(1.f, 2.f, 0.f), 1e-6f);
    // A ray going up and to +x at 45 degrees from the origin: the tube's point nearest it is x = 1 (its end: the
    // line through it would be met at x = 2)
    Vec3 r = normalize(Vec3{1, 1, 0});
    CHECK_VEC(arealight::segmentNearestRay({-1, 2, 0}, {1, 2, 0}, r), test::v3(1.f, 2.f, 0.f), 1e-5f);
    // ... a longer tube: x = 2, where the ray crosses it
    CHECK_VEC(arealight::segmentNearestRay({-3, 2, 0}, {3, 2, 0}, r), test::v3(2.f, 2.f, 0.f), 1e-5f);
    // A sphere 0.5 round, 4 m along x; a ray along +x a metre up (or straight up): its nearest point on the sphere
    CHECK_VEC(arealight::sphereNearestRay({4, -1, 0}, {1, 0, 0}, 0.5f), test::v3(4.f, -0.5f, 0.f), 1e-5f);
    CHECK_VEC(arealight::sphereNearestRay({4, 0, 0}, {1, 0, 0}, 0.5f), test::v3(4.f, 0.f, 0.f), 1e-5f);   // (on it)
}

TEST(area_light_highlight_scale) {
    CHECK_NEAR(arealight::highlightScale(0.1f, 0.f, 0.f, 3.f), 1.0, 1e-6);   // (a point: as it was)
    // Bigger lights, nearer, spread their highlight thinner; a rough surface's barely changes
    CHECK(arealight::highlightScale(0.1f, 0.2f, 0.f, 3.f) < 1.f);
    CHECK(arealight::highlightScale(0.1f, 0.2f, 0.f, 1.f) < arealight::highlightScale(0.1f, 0.2f, 0.f, 3.f));
    CHECK(arealight::highlightScale(0.1f, 0.f, 2.f, 3.f) < arealight::highlightScale(0.1f, 0.f, 0.5f, 3.f));
    CHECK(arealight::highlightScale(0.9f, 0.2f, 0.f, 3.f) > 0.9f);   // (0.93, against 0.56 at alpha 0.1)
    CHECK(arealight::highlightScale(0.1f, 0.5f, 2.f, 3.f) > 0.f);
}

// A rectangle light 2 x 1 m, its middle 3 m up, facing down (x along x, y along z)
TEST(area_light_rectangles) {
    const Vec3 c{0, 3, 0}, x{1, 0, 0}, y{0, 0, 1};
    // From the origin straight under it: its middle; from off to the side: its edge's point nearest
    CHECK_VEC(arealight::rectNearest(c, x, y, 1.f, 0.5f), test::v3(0.f, 3.f, 0.f), 1e-6f);
    CHECK_VEC(arealight::rectNearest(c - Vec3{4, 0, 0}, x, y, 1.f, 0.5f), test::v3(-3.f, 3.f, 0.f), 1e-6f);
    // A ray up at 45 degrees along x meets its plane at x = 3, past its edge: kept to x = 1
    CHECK_VEC(arealight::rectRayPoint(c, x, y, 1.f, 0.5f, normalize(Vec3{1, 1, 0})), test::v3(1.f, 3.f, 0.f), 1e-5f);
    // ... a steeper one meets it inside: where it meets
    const Vec3 steep = normalize(Vec3{0.2f, 1, 0.1f});
    const Vec3 at = arealight::rectRayPoint(c, x, y, 1.f, 0.5f, steep);
    CHECK_NEAR(at.y, 3.0, 1e-5);
    CHECK_NEAR(at.x, 0.6, 1e-4);
    CHECK_NEAR(at.z, 0.3, 1e-4);
    // A ray going down, away from it: a point of the rectangle all the same, on its plane
    const Vec3 away = arealight::rectRayPoint(c, x, y, 1.f, 0.5f, normalize(Vec3{1, -1, 0}));
    CHECK_NEAR(away.y, 3.0, 1e-5);
    CHECK(away.x <= 1.f + 1e-5f && away.x >= -1.f - 1e-5f);
}

// A rectangle's picture: its top-left corner as seen from in front (the light's side, its -z) is the picture's; the
// light right against it takes one texel's colour, from as far as its size the whole picture's average
TEST(area_light_picture) {
    Vec3 x{1, 0, 0}, y{0, 1, 0};   // (an entity unturned: lighting towards -z, seen from there looking along +z)
    float u = 0.f, v = 0.f;
    // Seen from -z looking along +z, the viewer's left is +x: the picture's left edge
    arealight::pictureUv({1.5f, 1.f, 0.f}, x, y, 3.f, 2.f, u, v);
    CHECK_NEAR(u, 0.0, 1e-6);
    CHECK_NEAR(v, 0.0, 1e-6);
    arealight::pictureUv({-1.5f, -1.f, 0.f}, x, y, 3.f, 2.f, u, v);
    CHECK_NEAR(u, 1.0, 1e-6);
    CHECK_NEAR(v, 1.0, 1e-6);
    arealight::pictureUv({0.f, 0.f, 0.f}, x, y, 3.f, 2.f, u, v);
    CHECK_NEAR(u, 0.5, 1e-6);
    CHECK(arealight::pictureLod(0.f, 3.f) == 0.f);
    CHECK_NEAR(arealight::pictureLod(3.f, 3.f), 8.0, 1e-5);    // (256 texels to 1)
    CHECK_NEAR(arealight::pictureLod(30.f, 3.f), 8.0, 1e-5);
    CHECK(arealight::pictureLod(0.3f, 3.f) > 4.f && arealight::pictureLod(0.3f, 3.f) < 5.f);
}
