// Temporal AA's history (render/taa_filter.h): the Catmull-Rom read and how much history is kept
#include "test.h"

#include "game/game.h"
#include "render/taa_filter.h"

TEST(taa_catmull_rom) {
    for (float f : {0.f, 0.1f, 0.25f, 0.5f, 0.8f, 1.f}) {
        std::array<float, 4> w = taa::catmullRom(f);
        CHECK_NEAR(w[0] + w[1] + w[2] + w[3], 1.0, 1e-6);   // (keeps a flat colour)
        // A straight ramp (texels at -1, 0, 1, 2) read back exactly where the sample is
        CHECK_NEAR(-w[0] + w[2] + 2.f * w[3], f, 1e-6);
    }
    std::array<float, 4> at = taa::catmullRom(0.f), next = taa::catmullRom(1.f);
    CHECK_NEAR(at[1], 1.0, 1e-6);   // (on a texel: that texel)
    CHECK_NEAR(next[2], 1.0, 1e-6);
    // Sharper than bilinear: halfway, the outer texels pull against it (negative lobes)
    std::array<float, 4> half = taa::catmullRom(0.5f);
    CHECK(half[0] < 0.f && half[3] < 0.f && half[1] > 0.5f);
}

TEST(taa_history_weight) {
    CHECK_NEAR(taa::historyWeight(0.9f, 0.f, 0.f), 0.9, 1e-6);   // (still and matching: all of it)
    CHECK(taa::historyWeight(0.9f, 4.f, 0.f) < 0.9f);
    CHECK(taa::historyWeight(0.9f, 16.f, 0.f) < taa::historyWeight(0.9f, 4.f, 0.f));
    CHECK_NEAR(taa::historyWeight(0.9f, 100.f, 0.f), 0.72, 1e-5);   // (fast: no less than 80 %)
    CHECK(taa::historyWeight(0.9f, 0.f, 0.5f) < 0.9f);
    CHECK_NEAR(taa::historyWeight(0.9f, 0.f, 3.f), taa::historyWeight(0.9f, 0.f, 1.f), 1e-6);   // (clamped)
    CHECK(taa::historyWeight(0.f, 5.f, 0.5f) == 0.f);   // (no history: none)
}

// The player's sharpness: a setting (0..1) with the engine's old default
TEST(taa_sharpness_setting) {
    bool found = false;
    for (const auto& [key, value] : Game::defaultSettings())
        if (key == "settings.sharpness") {
            found = true;
            CHECK_NEAR(value.asNumber(), 0.3, 1e-9);
        }
    CHECK(found);
}

// The clip box over the cross: a flat neighbourhood is a point; one bright outlier among five widens the range but
// the variance keeps the box well short of it (a firefly in the history isn't let through)
TEST(taa_neighbourhood_box) {
    Vec3 lo, hi;
    taa::neighbourhoodBox({Vec3{0.3f, 0.3f, 0.3f}, Vec3{0.3f, 0.3f, 0.3f}, Vec3{0.3f, 0.3f, 0.3f}, Vec3{0.3f, 0.3f, 0.3f},
                           Vec3{0.3f, 0.3f, 0.3f}}, lo, hi);
    CHECK_NEAR(lo.x, 0.3, 1e-6);
    CHECK_NEAR(hi.x, 0.3, 1e-6);
    taa::neighbourhoodBox({Vec3{0.2f, 0.2f, 0.2f}, Vec3{0.2f, 0.2f, 0.2f}, Vec3{0.2f, 0.2f, 0.2f}, Vec3{0.2f, 0.2f, 0.2f},
                           Vec3{0.9f, 0.9f, 0.9f}}, lo, hi);
    CHECK(lo.x >= 0.2f - 1e-6f);
    CHECK(hi.x < 0.9f && hi.x > 0.2f);
    CHECK(hi.x < 0.75f);
}

// The composite's sharpening: a flat patch stays as it is; a pixel brighter than its neighbours gets brighter (and
// more so the stronger), never past the range; at the range's ends (black, white) it holds back
TEST(taa_cas_sharpen) {
    CHECK_NEAR(taa::casSharpen(0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 1.f), 0.4, 1e-5);
    const float soft = taa::casSharpen(0.5f, 0.4f, 0.4f, 0.4f, 0.4f, 0.3f), strong = taa::casSharpen(0.5f, 0.4f, 0.4f, 0.4f, 0.4f, 1.f);
    CHECK(soft > 0.5f && strong > soft && strong < 1.f);
    CHECK(taa::casSharpen(0.3f, 0.4f, 0.4f, 0.4f, 0.4f, 1.f) < 0.3f);   // (darker than round it: darker)
    CHECK_NEAR(taa::casSharpen(0.f, 0.3f, 0.3f, 0.3f, 0.3f, 1.f), 0.0, 1e-6);   // (black: no room)
    CHECK(taa::casSharpen(0.99f, 0.5f, 0.5f, 0.5f, 0.5f, 1.f) <= 0.999f);
}
