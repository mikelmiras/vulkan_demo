// Render scale: dynamic resolution's choice of scale (render/renderer.h: nextDynamicScale)
#include "test.h"

#include "render/renderer.h"

TEST(render_scale_dynamic_choice) {
    // Over budget: down (in steps of 5 %), never under half; under budget: back up to the set scale, no further
    float s = 1.f;
    for (int i = 0; i < 40; ++i) s = Renderer::nextDynamicScale(s, 33.f, 16.7f, 1.f);   // (twice the budget)
    CHECK(s >= 0.5f && s <= 0.75f);
    for (int i = 0; i < 200; ++i) s = Renderer::nextDynamicScale(s, 3.f, 16.7f, 0.85f);
    CHECK_NEAR(s, 0.85, 1e-5);   // (cheap now: up to the set scale)
    CHECK(Renderer::nextDynamicScale(0.5f, 100.f, 16.7f, 1.f) == 0.5f);
    // Close to the budget: it holds (no change under a step)
    CHECK(Renderer::nextDynamicScale(0.8f, 15.5f, 16.7f, 1.f) == 0.8f);
    // A frame time that would want it about 70 %: it gets there and stays
    s = 1.f;
    auto cost = [](float scale) { return 30.f * scale * scale; };   // (GPU time going as the pixels)
    for (int i = 0; i < 100; ++i) s = Renderer::nextDynamicScale(s, cost(s), 16.7f, 1.f);
    CHECK(cost(s) <= 16.7f && cost(s) > 12.f);
    float settled = s;
    for (int i = 0; i < 20; ++i) s = Renderer::nextDynamicScale(s, cost(s), 16.7f, 1.f);
    CHECK(s == settled);
    CHECK(Renderer::nextDynamicScale(0.7f, 0.f, 16.7f, 1.f) == 0.7f);   // (no timestamps: as it is)
}
