// Screen-space GI's denoise (render/gi_denoise.h): taps on the same surface count, across an edge they don't
#include "test.h"

#include "render/gi_denoise.h"

TEST(gi_denoise_weights) {
    CHECK_NEAR(gidenoise::tapWeight(0, 0, 10.f, 10.f, 1.f), 1.0, 1e-6);   // (the centre)
    CHECK_NEAR(gidenoise::tapWeight(1, 1, 10.f, 10.f, 1.f), 0.25, 1e-6);  // (a corner, the same surface)
    // A step in depth (a box in front of a wall: 2 m nearer at 10 m) or a turn (a wall meeting the floor): next to
    // nothing
    CHECK(gidenoise::tapWeight(1, 0, 10.f, 8.f, 1.f) < 0.01f);
    CHECK(gidenoise::tapWeight(1, 0, 10.f, 10.f, 0.f) < 1e-6f);
    // A slope's gentle change of depth across a tap or two: still counts
    CHECK(gidenoise::tapWeight(1, 0, 10.f, 10.1f, 0.99f) > 0.3f);
}
