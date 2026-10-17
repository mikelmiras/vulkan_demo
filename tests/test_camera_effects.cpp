// Camera effects (RenderSettings: depth of field, motion blur): the focus distance (render/scene_draw.h's
// focusDistance), the settings saved with a scene and seen by undo
#include "test.h"

#include <cmath>

#include "render/camera_fx.h"
#include "render/scene_draw.h"
#include "scene/scene.h"
#include "scene/scene_diff.h"
#include "scene/serialization.h"

TEST(camera_effects_focus) {
    Scene s;
    Entity& car = s.create("Car");
    car.transform.position[0] = 3.f;
    car.transform.position[2] = -20.f;
    s.render.focusDistance = 7.f;
    // No target: the distance; a target: its distance along the view (from the camera at the origin looking -z)
    CHECK_NEAR(focusDistance(s, s.worldMatrices(), {0, 0, 0}, {0, 0, -1}), 7.0, 1e-6);
    s.render.focusTarget = car.id;
    CHECK_NEAR(focusDistance(s, s.worldMatrices(), {0, 0, 0}, {0, 0, -1}), 20.0, 1e-4);
    // Behind the camera: as near as can be; a target that's gone: the distance again
    CHECK_NEAR(focusDistance(s, s.worldMatrices(), {0, 0, 0}, {0, 0, 1}), 0.1, 1e-6);
    s.remove(car.id);
    CHECK_NEAR(focusDistance(s, s.worldMatrices(), {0, 0, 0}, {0, 0, -1}), 7.0, 1e-6);
}

TEST(camera_effects_saved) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Entity& hero = s.create("Hero");
    s.render.depthOfField = 0.6f;
    s.render.focusDistance = 12.f;
    s.render.focusTarget = hero.id;
    s.render.motionBlur = 0.4f;
    std::string error;
    auto path = test::tempPath("camera_fx.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    CHECK_NEAR(loaded.render.depthOfField, 0.6, 1e-6);
    CHECK_NEAR(loaded.render.focusDistance, 12.0, 1e-6);
    CHECK(loaded.render.focusTarget == hero.id && loaded.find(loaded.render.focusTarget));
    CHECK_NEAR(loaded.render.motionBlur, 0.4, 1e-6);
    Scene plain;
    CHECK(plain.render.depthOfField == 0.f && plain.render.motionBlur == 0.f && plain.render.focusTarget == kNoEntity);
    // Undo sees the edits
    Scene after = loaded;
    after.render.focusTarget = kNoEntity;
    CHECK(!diffScenes(loaded, after).empty());
}

TEST(camera_effects_sky_motion) {
    // A camera looking down -z; turned 10 degrees to the left last frame: the sky ahead was to its right then
    Mat4 proj = perspective(60.f * DEG2RAD, 16.f / 9.f, 0.1f, 500.f, true);
    Mat4 now = proj * lookAt({5, 2, 3}, {5, 2, 2}, {0, 1, 0});
    float h[9];
    camerafx::skyMotion(now, now, h);   // (still: none)
    for (int k = 0; k < 9; ++k) CHECK_NEAR(h[k], k % 4 == 0 ? 1.0 : 0.0, 1e-5);
    Mat4 moved = proj * lookAt({50, 8, -30}, {50, 8, -31}, {0, 1, 0});   // (moved, not turned: the sky stays put)
    camerafx::skyMotion(now, moved, h);
    for (int k = 0; k < 9; ++k) CHECK_NEAR(h[k] / h[8], k % 4 == 0 ? 1.0 : 0.0, 1e-4);
    float a = 10.f * DEG2RAD;
    Mat4 turned = proj * lookAt({5, 2, 3}, {5 - std::sin(a), 2, 3 - std::cos(a)}, {0, 1, 0});
    camerafx::skyMotion(now, turned, h);
    // The middle of the screen now (straight down -z) was right of the middle last frame, at tan(10) / tan(fovX / 2)
    float x = h[2] / h[8], y = h[5] / h[8];
    float tanX = std::tan(30.f * DEG2RAD) * 16.f / 9.f;
    CHECK_NEAR(x, std::tan(a) / tanX, 1e-3);
    CHECK_NEAR(y, 0.0, 1e-4);
}

// Motion blur's taps (camera_fx.glsl's weights): a moving thing smears over a still background beside it (its tap,
// in front, reaches a still pixel within its motion), not further; a still thing in front of a moving one isn't
// smeared over by it
TEST(camera_effects_motion_weights) {
    // A still background pixel (here 0), a car's tap 6 px away moving 10 px a frame, in front: counts
    CHECK(camerafx::motionTapWeight(6.f, 0.f, 10.f, 1.f) > 0.3f);
    // ... 14 px away: beyond its smear
    CHECK_NEAR(camerafx::motionTapWeight(14.f, 0.f, 10.f, 1.f), 0.0, 1e-6);
    // A still post in front of a moving pixel's tap: the tap behind, outside this (still) pixel's reach: nothing
    CHECK_NEAR(camerafx::motionTapWeight(6.f, 0.f, 10.f, 0.f), 0.0, 1e-6);
    // A moving pixel and its own moving neighbour (both spanning the gap): more than either alone
    CHECK(camerafx::motionTapWeight(3.f, 10.f, 10.f, 0.5f) > camerafx::motionTapWeight(3.f, 10.f, 10.f, 1.f) - 1e-6f);
    CHECK(camerafx::motionTapWeight(3.f, 10.f, 10.f, 0.5f) > 1.f);
    // Nearer taps count more along a smear
    CHECK(camerafx::motionTapWeight(2.f, 0.f, 10.f, 1.f) > camerafx::motionTapWeight(8.f, 0.f, 10.f, 1.f));
}

// Depth of field's near field: covering half the disc, half over the background; all of it, all; none, none
TEST(camera_effects_near_coverage) {
    CHECK_NEAR(camerafx::nearCoverage(12.f, 24), 0.5, 1e-6);
    CHECK_NEAR(camerafx::nearCoverage(24.f, 24), 1.0, 1e-6);
    CHECK_NEAR(camerafx::nearCoverage(30.f, 24), 1.0, 1e-6);
    CHECK_NEAR(camerafx::nearCoverage(0.f, 24), 0.0, 1e-6);
    CHECK_NEAR(camerafx::nearCoverage(5.f, 0), 0.0, 1e-6);
}
