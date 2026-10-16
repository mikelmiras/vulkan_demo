// Camera effects (RenderSettings: depth of field, motion blur): the focus distance (render/scene_draw.h's
// focusDistance), the settings saved with a scene and seen by undo
#include "test.h"

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
