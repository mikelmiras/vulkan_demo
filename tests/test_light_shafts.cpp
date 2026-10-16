// Light shafts (render/light_shafts.h): what the march gets from the settings and the lights; saved with a scene
#include "test.h"

#include <algorithm>

#include "render/light_shafts.h"
#include "scene/scene.h"
#include "scene/serialization.h"

TEST(light_shafts_chosen) {
    // Off without strength, with volumetric fog (it has its own) and at low quality
    CHECK(shafts::choose(0.f, false, 2, {}).density == 0.f);
    CHECK(shafts::choose(1.f, true, 2, {}).density == 0.f);
    CHECK(shafts::choose(1.f, false, 0, {}).density == 0.f);
    // On: a light haze, more steps at high quality; stronger, denser (capped at 2)
    shafts::Params p = shafts::choose(1.f, false, 2, {0.f, 1.f, 1.f, 1.f});
    CHECK(p.density > 0.f && p.steps == 24);
    CHECK(shafts::choose(1.f, false, 1, {}).steps == 16);
    CHECK_NEAR(shafts::choose(2.f, false, 1, {}).density, 2.0 * p.density, 1e-7);
    CHECK_NEAR(shafts::choose(5.f, false, 1, {}).density, 2.0 * p.density, 1e-7);
    // Every lamp that shows, up to four: the four that show most, in the lights' order; none if none show
    CHECK(p.lampCount == 3 && p.lamps[0] == 1 && p.lamps[1] == 2 && p.lamps[2] == 3 && p.lamps[3] == -1);
    shafts::Params four = shafts::choose(1.f, false, 2, {0.1f, 5.f, 0.f, 3.f, 0.2f, 4.f, 2.f});
    CHECK(four.lampCount == 4 && four.lamps[0] == 1 && four.lamps[1] == 3 && four.lamps[2] == 5 && four.lamps[3] == 6);
    shafts::Params none = shafts::choose(1.f, false, 2, {0.f, 0.f});
    CHECK(none.lampCount == 0 && none.lamps[0] == -1);
    // A street lamp (bright, wide) down the road over a tail light (dim, small) by the camera
    CHECK(shafts::prominence(20.f, 12.f, 25.f) > shafts::prominence(2.f, 2.f, 3.f));
    CHECK(shafts::prominence(20.f, 12.f, 5.f) > shafts::prominence(20.f, 12.f, 50.f));
    CHECK(shafts::prominence(0.f, 12.f, 5.f) == 0.f && shafts::prominence(5.f, 0.f, 5.f) == 0.f);
}

// A point light's halo along a ray: the closed form against summing it in small steps
TEST(light_shafts_halo) {
    auto summed = [](float tc, float h, float t0, float t1, float soft) {
        double sum = 0.0;
        const int n = 200000;
        for (int i = 0; i < n; ++i) {
            double t = t0 + (t1 - t0) * (i + 0.5) / n, s = t - tc;
            sum += 1.0 / (h * h + s * s + soft * soft);
        }
        return sum * (t1 - t0) / n;
    };
    for (float h : {0.f, 0.3f, 2.f, 10.f})
        for (float tc : {-3.f, 0.f, 5.f, 40.f}) {
            float closed = shafts::halo(tc, h, 0.f, 20.f, 0.5f);
            CHECK_NEAR(closed, summed(tc, h, 0.f, 20.f, 0.5f), 1e-3 * std::max(1.0, (double)closed));
        }
    CHECK(shafts::halo(5.f, 1.f, 3.f, 3.f, 0.5f) == 0.f);   // (no length: none)
    // Straight through the bulb, the softened core: about pi / soft over a long ray
    CHECK_NEAR(shafts::halo(50.f, 0.f, 0.f, 100.f, 0.5f), 3.14159265 / 0.5, 0.05);
    // Further from the ray, fainter
    CHECK(shafts::halo(10.f, 3.f, 0.f, 20.f, 0.5f) < shafts::halo(10.f, 1.f, 0.f, 20.f, 0.5f));
}

TEST(light_shafts_saved) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    s.render.lightShafts = 1.2f;
    std::string error;
    auto path = test::tempPath("shafts.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    CHECK_NEAR(loaded.render.lightShafts, 1.2, 1e-6);
    CHECK(Scene{}.render.lightShafts == 0.f);   // (off unless asked for)
}
