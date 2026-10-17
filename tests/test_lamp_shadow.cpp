// Lamps' soft shadows (render/lamp_shadow.h): a blocker's depth back to its place along the ray, the penumbra
#include "test.h"

#include "core/vecmath.h"
#include "render/lamp_shadow.h"

TEST(lamp_shadow_blocker_fraction) {
    // A spot light at (1, 3, 2) looking down -z with a 60 degree cone (near 0.1, far 20); a point 10 m along a ray off
    // its axis, a blocker 4 m along the same ray: 0.4 of the way
    const Vec3 light{1, 3, 2};
    const Mat4 viewProj = perspective(60.f * DEG2RAD, 1.f, 0.1f, 20.f, true) * lookAt(light, light + Vec3{0, 0, -1}, {0, 1, 0});
    const Vec3 dir = normalize(Vec3{0.2f, -0.1f, -1.f});
    struct Clip {
        float x, y, z, w;
    };
    auto clip = [&](Vec3 p) {
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = viewProj.m[0 * 4 + r] * p.x + viewProj.m[1 * 4 + r] * p.y + viewProj.m[2 * 4 + r] * p.z + viewProj.m[3 * 4 + r];
        return Clip{c[0], c[1], c[2], c[3]};
    };
    for (float along : {4.f, 1.f, 9.5f}) {
        const Clip point = clip(light + dir * 10.f), blocker = clip(light + dir * along), atLight = clip(light);
        const float d = blocker.z / blocker.w;
        CHECK_NEAR(lampshadow::blockerFraction(d, point.z, point.w, atLight.z), along / 10.f, 1e-3);
    }
}

TEST(lamp_shadow_penumbra) {
    CHECK_NEAR(lampshadow::penumbra(0.3f, 1.f), 0.0, 1e-6);    // (the blocker at the point: sharp)
    CHECK_NEAR(lampshadow::penumbra(0.3f, 0.5f), 0.3, 1e-6);   // (half way: as wide as the light)
    CHECK(lampshadow::penumbra(0.3f, 0.1f) > lampshadow::penumbra(0.3f, 0.5f));   // (near the light: wider)
    CHECK(lampshadow::penumbra(0.f, 0.1f) == 0.f);
}
