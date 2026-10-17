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
