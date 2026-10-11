// Scene format migration: a genuine file of every format the engine has written (tests/assets/formats, each saved
// by the engine of its time) still loads, keeps what its format could hold, and survives a save in today's format
#include "test.h"

#include <functional>

#include "scene/serialization.h"

#ifdef VKWORLD_TEST_ASSETS

namespace {

std::string fixture(const char* name) { return std::string(VKWORLD_TEST_ASSETS) + "/formats/" + name; }

template <class F> int count(const Scene& s, F&& pred) {
    int n = 0;
    for (const Entity& e : s.entities) n += pred(e) ? 1 : 0;
    return n;
}

bool hasAction(const Scene& s, const char* name) {
    for (const InputAction& a : s.inputActions)
        if (a.name == name) return true;
    return false;
}

// Loads `name`, runs `check` on it, then saves it in today's format, loads that and checks again
void roundTrip(const char* name, const std::function<void(const Scene&)>& check) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    std::string error;
    if (!loadScene(fixture(name), s, meshes, textures(), error)) {
        test::fail(__FILE__, __LINE__, std::string(name) + ": " + error);
        return;
    }
    CHECK(s.entities.size() > 20);
    CHECK(countMissingBehaviours(s) == 0);   // every behaviour type it uses still exists
    check(s);
    auto again = test::tempPath(std::string("resaved_") + name + ".vkw").string();
    REQUIRE(saveScene(again, s, meshes, textures(), error));
    Scene t;
    REQUIRE(loadScene(again, t, meshes, textures(), error));
    CHECK(t.entities.size() == s.entities.size());
    CHECK(t.prefabs.size() == s.prefabs.size());
    check(t);
}

}  // namespace

TEST(format_v2_text) {
    roundTrip("scene_v2.txt", [](const Scene& s) {
        CHECK(count(s, [](const Entity& e) { return e.camera.has_value(); }) >= 1);
        CHECK(count(s, [](const Entity& e) { return !e.behaviours.empty(); }) > 5);   // its spin / bob, as behaviours
    });
}

TEST(format_v3_entities) {
    roundTrip("scene_v3.vkw", [](const Scene& s) {
        CHECK(count(s, [](const Entity& e) { return e.parent != kNoEntity; }) > 10);   // a hierarchy
    });
}

TEST(format_v4_prefabs) {
    roundTrip("scene_v4.vkw", [](const Scene& s) { CHECK(!s.prefabs.empty()); });
}

TEST(format_v5_textures_and_lights) {
    roundTrip("scene_v5.vkw", [](const Scene& s) {
        CHECK(count(s, [](const Entity& e) { return e.meshRenderer && e.meshRenderer->texture >= 0; }) > 5);
        CHECK(count(s, [](const Entity& e) { return e.light.has_value(); }) >= 2);
    });
}

TEST(format_v6_prefab_links) {
    roundTrip("scene_v6.vkw", [](const Scene& s) {
        CHECK(count(s, [](const Entity& e) { return e.prefab.has_value(); }) >= 5);
    });
}

TEST(format_v7_rigid_bodies) {
    roundTrip("scene_v7.vkw", [](const Scene& s) {
        CHECK(count(s, [](const Entity& e) { return e.rigidBody.has_value(); }) >= 5);
    });
}

TEST(format_v8_audio) {
    roundTrip("scene_v8.vkw", [](const Scene& s) {
        CHECK(count(s, [](const Entity& e) { return e.audioSource.has_value(); }) >= 5);
        CHECK(hasAction(s, "quicksave") && hasAction(s, "pause"));   // actions added since: given their defaults
    });
}

TEST(format_v9_to_v11) {
    for (const char* name : {"scene_v9.vkw", "scene_v10.vkw", "scene_v11.vkw"})
        roundTrip(name, [](const Scene& s) {
            CHECK(hasAction(s, "quicksave") && hasAction(s, "quickload") && hasAction(s, "pause"));
            CHECK(count(s, [](const Entity& e) { return e.rigidBody.has_value(); }) >= 5);
        });
}

#endif
