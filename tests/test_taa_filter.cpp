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
