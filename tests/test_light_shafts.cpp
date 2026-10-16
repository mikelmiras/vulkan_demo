// Light shafts (render/light_shafts.h): what the march gets from the settings and the lights; saved with a scene
#include "test.h"

#include "render/light_shafts.h"
#include "scene/scene.h"
#include "scene/serialization.h"

TEST(light_shafts_chosen) {
    // Off without strength, with volumetric fog (it has its own) and at low quality
    CHECK(shafts::choose(0.f, false, 2, {}).density == 0.f);
    CHECK(shafts::choose(1.f, true, 2, {}).density == 0.f);
    CHECK(shafts::choose(1.f, false, 0, {}).density == 0.f);
    // On: a light haze, more steps at high quality; stronger, denser (capped at 2)
    shafts::Params p = shafts::choose(1.f, false, 2, {false, true, true, true});
    CHECK(p.density > 0.f && p.steps == 24);
    CHECK(shafts::choose(1.f, false, 1, {}).steps == 16);
    CHECK_NEAR(shafts::choose(2.f, false, 1, {}).density, 2.0 * p.density, 1e-7);
    CHECK_NEAR(shafts::choose(5.f, false, 1, {}).density, 2.0 * p.density, 1e-7);
    // The first two shadowed spot lights (the nearest), none if there are none
    CHECK(p.lamps[0] == 1 && p.lamps[1] == 2);
    shafts::Params none = shafts::choose(1.f, false, 2, {false, false});
    CHECK(none.lamps[0] == -1 && none.lamps[1] == -1);
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
