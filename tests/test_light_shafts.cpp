// Light shafts (render/light_shafts.h): where the sun is on screen and how much its shafts show; saved with a scene
#include "test.h"

#include <cmath>

#include "render/light_shafts.h"
#include "scene/scene.h"
#include "scene/serialization.h"

TEST(light_shafts_sun_on_screen) {
    // A camera at the origin looking down -z, a 1600 x 900 viewport at (100, 50)
    Mat4 vp = perspective(60.f * DEG2RAD, 16.f / 9.f, 0.1f, 500.f, true) * lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    auto at = [&](Vec3 toSun) { return shafts::sunOnScreen(vp, {0, 0, 0}, normalize(toSun) * -1.f, 100.f, 50.f, 1600.f, 900.f); };
    // Straight ahead: the viewport's middle, fully shown
    shafts::SunOnScreen ahead = at({0, 0, -1});
    CHECK_NEAR(ahead.x, 900.0, 0.5);
    CHECK_NEAR(ahead.y, 500.0, 0.5);
    CHECK_NEAR(ahead.fade, 1.0, 1e-6);
    // Up and to the left: higher (smaller y) and left of the middle, still in view
    shafts::SunOnScreen upLeft = at({-0.3f, 0.2f, -1.f});
    CHECK(upLeft.x < 900.f && upLeft.y < 500.f && upLeft.y > 50.f && upLeft.fade == 1.f);
    // Just above the top edge: fading; far beyond it or behind the camera: none
    float topEdge = std::tan(30.f * DEG2RAD);
    shafts::SunOnScreen above = at({0.f, topEdge * 1.15f, -1.f});
    CHECK(above.y < 50.f && above.fade > 0.f && above.fade < 1.f);
    CHECK(at({0.f, 3.f, -1.f}).fade == 0.f);
    CHECK(at({0.f, 0.3f, 1.f}).fade == 0.f);
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
