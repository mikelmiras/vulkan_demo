// Loading: scenes read in the background (assets merged into the game's libraries), loading screens, streaming
#include "test.h"

#include <chrono>
#include <thread>

#include "assets/gltf.h"
#include "game/game.h"
#include "game/scene_loader.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

void run(Game& g, int frames) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    }
}

// Runs frames until the game is done loading (or a few seconds pass)
bool runUntilLoaded(Game& g) {
    for (int i = 0; i < 500 && g.loading(); ++i) {
        run(g, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !g.loading();
}

struct Counter : BehaviourBase<Counter> {
    static constexpr const char* kTypeName = "TestLoadCounter";
    std::string next;     // loadScene / streamIn this on the first update
    bool stream = false;
    Vec3 at;
    EntityId chunk = kNoEntity;
    int updates = 0;
    void fields(FieldVisitor& v) override {
        v.field("next", next);
        v.field("stream", stream);
        v.field("at", at);
    }
    void start(GameContext& ctx, Entity&) override { ctx.state().add("starts", 1.f); }
    void update(GameContext& ctx, Entity&) override {
        if (++updates == 1 && !next.empty()) {
            if (stream) chunk = ctx.streamIn(next, at);
            else ctx.loadScene(next);
        }
    }
};

struct Remover : BehaviourBase<Remover> {   // streams `chunk` out on its next update
    static constexpr const char* kTypeName = "TestRemover";
    EntityId chunk = kNoEntity;
    void update(GameContext& c, Entity&) override {
        if (chunk != kNoEntity) c.streamOut(chunk);
        chunk = kNoEntity;
    }
};

void registerCounter() {
    static bool done = false;
    if (!done) registerBehaviour<Counter>("test");
    done = true;
}

// A scene file: a "Box" (with a Counter, and a child "Lid"), a "Spark" prefab
std::string writeScene(const std::string& name, MeshLibrary& meshes, const std::string& next = "", bool stream = false,
                       Vec3 at = {}) {
    registerCounter();
    Scene s;
    Entity& box = s.create("Box");
    box.transform.position[1] = 1.f;
    box.meshRenderer = MeshRenderer{};
    auto c = std::make_unique<Counter>();
    c->next = next;
    c->stream = stream;
    c->at = at;
    box.behaviours.emplace_back(std::move(c));
    EntityId boxId = box.id;
    Entity& lid = s.create("Lid");
    lid.parent = boxId;
    lid.transform.position[1] = 0.5f;
    Prefab p;
    p.name = "Spark";
    Entity spark;
    spark.id = 1;
    spark.name = "Spark";
    p.entities.push_back(spark);
    s.addPrefab(p);
    std::string path = test::tempPath(name + ".vkw").string(), error;
    if (!saveScene(path, s, meshes, textures(), error)) std::printf("    %s\n", error.c_str());
    return path;
}

}  // namespace

// The loader runs on a worker thread; finish() hands over the scene
TEST(loading_scene_loader) {
    MeshLibrary meshes = buildMeshLibrary();
    std::string path = writeScene("loader_plain", meshes);
    SceneLoader loader;
    CHECK(!loader.busy());
    loader.start(path);
    CHECK(loader.busy());
    Scene s;
    std::string error;
    REQUIRE(loader.finish(s, meshes, textures(), error));
    CHECK(!loader.busy());
    CHECK(s.findByName("Box") && s.findByName("Lid"));
    CHECK(s.findPrefab("Spark") != nullptr);
    loader.start(test::tempPath("no_such_scene.vkw").string());
    CHECK(!loader.finish(s, meshes, textures(), error));
    CHECK(error.find("cannot open") != std::string::npos);
}

#ifdef VKWORLD_TEST_ASSETS
// Imported meshes, textures and clips move into the game's libraries: reused when already there, ids remapped
TEST(loading_merges_assets) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Prefab bar;
    std::string error, warning;
    REQUIRE(importGltfPrefab(std::string(VKWORLD_TEST_ASSETS) + "/bending_bar.gltf", meshes, textures(), bar, error, warning));
    for (Entity& e : s.instantiate(bar)) s.add(std::move(e));
    std::string path = test::tempPath("loader_bar.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));

    // Into a library that has other meshes first: the ids move
    MeshLibrary other = buildMeshLibrary();
    std::vector<Vertex> tri(3);
    tri[1].pos = {1, 0, 0};
    tri[2].pos = {0, 1, 0};
    addMesh(other, "Unrelated", "unrelated.obj", tri, {0, 1, 2});
    other.animations.push_back(AnimationClip{"bending_bar/Bend", "elsewhere.glb#anim0", 1.f, {}});   // a name clash
    SceneLoader loader;
    loader.start(path);
    Scene loaded;
    REQUIRE(loader.finish(loaded, other, textures(), error));
    const Entity* body = loaded.findByName("Body");
    REQUIRE(body && body->meshRenderer);
    const Mesh& m = other.get(body->meshRenderer->mesh);
    CHECK(m.skinned);
    CHECK(m.source.find("bending_bar.gltf") != std::string::npos);
    CHECK(body->meshRenderer->mesh > other.find("Unrelated"));
    const Entity* root = loaded.findByName("bending_bar");
    REQUIRE(root && root->animator);
    const AnimationClip* clip = other.findAnimation(root->animator->clip);
    REQUIRE(clip);
    CHECK(clip->source.find("bending_bar.gltf") != std::string::npos);   // renamed, not the clashing one
    CHECK(root->animator->clip != "bending_bar/Bend");

    // Loading it again reuses everything
    int meshCount = other.count();
    size_t clipCount = other.animations.size();
    loader.start(path);
    REQUIRE(loader.finish(loaded, other, textures(), error));
    CHECK(other.count() == meshCount);
    CHECK(other.animations.size() == clipCount);
    CHECK(loaded.findByName("Body")->meshRenderer->mesh == body->meshRenderer->mesh);
}
#endif

// With asynchronous loading, loadScene() freezes the game until the next scene is in, behind a loading screen
TEST(loading_async_scene_change) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    std::string b = writeScene("async_b", meshes);
    std::string a = writeScene("async_a", meshes, b);
    Scene s;
    std::string error;
    REQUIRE(loadScene(a, s, meshes, textures(), error));
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.setAsyncLoading(true);
    g.start(s, meshes);
    run(g, 1);   // the first update asks for b
    CHECK(g.loading());
    unsigned long long tick = g.tick();
    REQUIRE(runUntilLoaded(g));
    CHECK(g.tick() <= tick + 1);   // no updates while it loaded (the frame it arrived in runs one)
    CHECK(g.scenePath() == b);
    CHECK_NEAR(g.state().number("starts"), 2.0, 0.0);   // a's box, then b's
    run(g, 3);
    CHECK(g.tick() > tick);
}

// Streaming: another scene's entities join this one under a root at the offset; streamOut removes them
TEST(loading_streaming) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    std::string part = writeScene("stream_part", meshes);
    std::string main = writeScene("stream_main", meshes, part, true, {10, 0, 0});
    for (bool async : {false, true}) {
        Scene s;
        std::string error;
        REQUIRE(loadScene(main, s, meshes, textures(), error));
        s.prefabs.clear();   // so the streamed file's prefab gets added
        Game g;
        g.setAssetLibraries(meshes, textures());
        g.setAsyncLoading(async);
        g.start(s, meshes);
        run(g, 1);
        if (async) {
            CHECK(!g.loading());   // streaming doesn't freeze the game
            g.finishLoading();
        }
        auto& counter = static_cast<Counter&>(*s.entities[0].behaviours[0]);
        REQUIRE(counter.chunk != kNoEntity);
        const Entity* root = s.find(counter.chunk);
        REQUIRE(root);
        CHECK(root->name == "Chunk stream_part");
        // Two boxes now: ours and the streamed one, under the chunk at x = 10
        const Entity* streamed = nullptr;
        for (const Entity& e : s.entities)
            if (e.name.rfind("Box", 0) == 0 && e.parent == counter.chunk) streamed = &e;
        REQUIRE(streamed);
        Mat4 w = s.worldMatrix(streamed->id);
        CHECK_NEAR(w.m[12], 10.0, 1e-5);
        CHECK_NEAR(w.m[13], 1.0, 1e-5);
        bool lid = false;
        for (const Entity& e : s.entities) lid = lid || (e.name.rfind("Lid", 0) == 0 && e.parent == streamed->id);
        CHECK(lid);
        CHECK_NEAR(g.state().number("starts"), 2.0, 0.0);   // the streamed box's behaviour started
        CHECK(s.findPrefab("Spark") != nullptr);
        size_t count = s.entities.size();
        // Out again
        EntityId chunk = counter.chunk;
        auto r = std::make_unique<Remover>();
        r->chunk = chunk;
        s.entities[0].behaviours.emplace_back(std::move(r));
        run(g, 2);
        CHECK(s.find(chunk) == nullptr);
        CHECK(s.entities.size() == count - 3);
    }
}

// The StreamingZone streams its scene in while the player is near and out once it's gone
TEST(loading_streaming_zone) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    std::string part = writeScene("zone_part", meshes);
    Scene s;
    Entity& player = s.create("Player");
    player.transform.position[0] = 100.f;
    Entity& zone = s.create("Zone");
    zone.transform.position[2] = 5.f;
    std::unique_ptr<Behaviour> b = createBehaviour("StreamingZone");
    REQUIRE(b);
    setField(*b, "scene", part);
    setField(*b, "radius", 20.f);
    setField(*b, "margin", 5.f);
    zone.behaviours.emplace_back(std::move(b));
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.start(s, meshes);
    auto chunkCount = [&] {
        int n = 0;
        for (const Entity& e : s.entities) n += e.name.rfind("Chunk", 0) == 0;
        return n;
    };
    run(g, 2);
    CHECK(chunkCount() == 0);
    s.findByName("Player")->transform.position[0] = 10.f;   // within 20 m
    run(g, 2);
    CHECK(chunkCount() == 1);
    s.findByName("Player")->transform.position[0] = 22.f;   // inside the margin: stays
    run(g, 2);
    CHECK(chunkCount() == 1);
    s.findByName("Player")->transform.position[0] = 40.f;   // gone
    run(g, 2);
    CHECK(chunkCount() == 0);
}

// The loader makes the physics world too; the game takes it as it is (bodies there from the first update)
TEST(loading_physics_made_by_the_loader) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene level;
    Entity& floor = level.create("Floor");
    floor.transform.scale[0] = floor.transform.scale[2] = 20.f;
    floor.collider = Collider{};
    Entity& ball = level.create("Ball");
    ball.transform.position[1] = 5.f;
    ball.meshRenderer = MeshRenderer{};
    ball.meshRenderer->mesh = (int)MeshKind::Sphere;
    Collider sphere;
    sphere.shape = Collider::Shape::Sphere;
    ball.collider = sphere;
    ball.rigidBody = RigidBody{};
    std::string path = test::tempPath("loader_physics.vkw").string(), error;
    REQUIRE(saveScene(path, level, meshes, textures(), error));

    SceneLoader loader;
    loader.start(path, true);
    Physics physics;
    Scene s;
    REQUIRE(loader.finish(s, meshes, textures(), error, &physics));
    CHECK(physics.bodyCount() == 2);
    CHECK_NEAR(loader.progress(), 1.0, 1e-6);

    Scene empty;
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.setAsyncLoading(true);
    g.start(empty, meshes);
    g.loadScene(path);
    CHECK(g.loading());
    g.finishLoading();
    REQUIRE(!g.loading());
    CHECK(g.loadError().empty());
    CHECK(g.physics().bodyCount() == 2);
    for (int i = 0; i < 60; ++i) {
        ImGui::NewFrame();
        g.step();
        ImGui::EndFrame();
    }
    CHECK(empty.findByName("Ball")->transform.position[1] < 4.f);   // it falls
    g.loadScene(test::tempPath("loader_missing.vkw").string());
    g.finishLoading();
    CHECK(!g.loadError().empty());
}
