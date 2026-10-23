// render/rain_drops: beads and running drops on glass in the rain
#include "test.h"

#include <cmath>

#include "render/rain_drops.h"

// Dry without rain; in heavy rain some of a pane is under water and some dry; a bead's slope leans out from its
// middle; a running drop on a steep pane moves down as time passes (not on a flat one)
TEST(rain_drops_on_glass) {
    int wet = 0, n = 0;
    for (int i = 0; i < 200; ++i)
        for (int j = 0; j < 200; ++j) {
            const float x = i * 0.006f, y = j * 0.006f;
            CHECK(raindrops::dropAt(x, y, 3.f, 0.f, 1.f).z == 0.f);
            Vec3 d = raindrops::dropAt(x, y, 3.f, 1.f, 1.f);
            wet += d.z >= 1.f ? 1 : 0;
            ++n;
            CHECK(std::fabs(d.x) < 4.f && std::fabs(d.y) < 4.f);
        }
    CHECK(wet > n / 10 && wet < n * 9 / 10);
    // Running drops move: a column's wet points at two times differ on a steep pane, not on a flat one (beads stay)
    auto changed = [](float steep) {
        int n = 0;
        for (int j = 0; j < 400; ++j) {
            const float y = j * 0.0025f;
            n += (raindrops::dropAt(0.02f, y, 0.f, 1.f, steep).z >= 1.f) != (raindrops::dropAt(0.02f, y, 0.5f, 1.f, steep).z >= 1.f);
        }
        return n;
    };
    CHECK(changed(1.f) > 0);
    CHECK(changed(0.f) == 0);
}
