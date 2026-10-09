// Prefabs, spawning, and the behaviours built on them
#include "test.h"

#include "core/json.h"
#include "game/game.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

// A prefab "Thing": root (scaled, with a mesh) and one child
Scene sceneWithPrefab() {
    Scene s;
    EntityId root = s.create("Thing").id;
    s.find(root)->transform.position[0] = 7;   // zeroed in the prefab
    s.find(root)->transform.scale[0] = 2;
    s.find(root)->meshRenderer = MeshRenderer{};
    EntityId child = s.create("Thing Child", root).id;
    s.find(child)->transform.position[1] = 1;
    s.prefabs.push_back(s.makePrefab(root, "Thing"));
    return s;
}

void run(Game& g, int frames) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    }
}

// Records what it sees, to check when spawned entities start and become visible
struct Probe : BehaviourBase<Probe> {
    static constexpr const char* kTypeName = "TestProbe";
    int starts = 0;
    void start(GameContext&, Entity&) override { ++starts; }
};

struct SpawnOnce : BehaviourBase<SpawnOnce> {
    static constexpr const char* kTypeName = "TestSpawnOnce";
    EntityId spawned = kNoEntity;
    bool inSceneDuringUpdate = true, foundDuringUpdate = false;
    void update(GameContext& ctx, Entity&) override {
        if (spawned) return;
        spawned = ctx.spawn("Thing", {1, 2, 3}, {1, 0, 0});
        inSceneDuringUpdate = ctx.scene().find(spawned) != nullptr;
        foundDuringUpdate = ctx.find(spawned) != nullptr;
    }
};

struct Bad : BehaviourBase<Bad> {
    static constexpr const char* kTypeName = "TestBad";
    EntityId got = 99;
    void start(GameContext& ctx, Entity&) override { got = ctx.spawn("Nope", {}); }
};

}  // namespace

TEST(prefab_make_and_instantiate) {
    Scene s = sceneWithPrefab();
    const Prefab& p = s.prefabs[0];
    REQUIRE(p.entities.size() == 2);
    CHECK(p.entities[0].parent == kNoEntity);
    CHECK(p.entities[0].transform.position[0] == 0.f);
    CHECK(p.entities[1].parent == p.entities[0].id);

    std::vector<Entity> a = s.instantiate(p), b = s.instantiate(p);
    REQUIRE(a.size() == 2 && b.size() == 2);
    CHECK(a[0].id != b[0].id && a[1].id != b[1].id);
    CHECK(a[1].parent == a[0].id && b[1].parent == b[0].id);
    for (const Entity& e : a) CHECK(!s.find(e.id));   // not added yet, ids reserved
    for (Entity& e : a) s.add(std::move(e));
    for (Entity& e : b) s.add(std::move(e));
    CHECK(s.entities.size() == 6);
    CHECK(s.find(b[0].id) && s.find(b[0].id)->transform.scale[0] == 2.f);
    CHECK(s.uniquePrefabName("Thing") == "Thing 2");
    CHECK(s.uniquePrefabName("") == "Prefab");
}

TEST(spawn_is_deferred_and_starts_behaviours) {
    test::ImGuiContextGuard imgui;
    Scene s = sceneWithPrefab();
    s.prefabs[0].entities[1].behaviours.emplace_back(std::make_unique<Probe>());
    EntityId spawnerId = s.create("Spawner").id;
    s.find(spawnerId)->behaviours.emplace_back(std::make_unique<SpawnOnce>());
    Game g;
    g.start(s, meshes());
    g.step();
    SpawnOnce* so = s.find(spawnerId)->behaviour<SpawnOnce>();
    REQUIRE(so->spawned != kNoEntity);
    CHECK(!so->inSceneDuringUpdate);   // deferred...
    CHECK(so->foundDuringUpdate);      // ...but findable through the context
    const Entity* root = s.find(so->spawned);
    REQUIRE(root);                     // added after the update
    Mat4 m = s.worldMatrix(root->id);
    CHECK_VEC(test::v3(m.m[12], m.m[13], m.m[14]), test::v3(1, 2, 3), 1e-5f);
    CHECK_VEC(normalize(transformDir(m, {0, 0, -1})), test::v3(1, 0, 0), 1e-5f);   // faces `forward`
    CHECK_NEAR(length(transformDir(m, {1, 0, 0})), 2.f, 1e-4);                     // keeps the prefab's scale
    std::vector<EntityId> kids = s.childrenOf(root->id);
    REQUIRE(kids.size() == 1);
    CHECK(s.find(kids[0])->behaviour<Probe>()->starts == 1);
}

TEST(spawn_unknown_prefab_logs) {
    test::ImGuiContextGuard imgui;
    Scene s;
    s.create("X").behaviours.emplace_back(std::make_unique<Bad>());
    Game g;
    g.start(s, meshes());
    CHECK(s.entities[0].behaviour<Bad>()->got == kNoEntity);
    REQUIRE(!g.log().empty());
    CHECK(g.log().back().second.find("Nope") != std::string::npos);
}

TEST(shooter_projectile_destructible_drop) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = buildDefaultScene();
    // A destructible crate straight ahead of the player, dropping a gem
    EntityId crate = s.create("Crate").id;
    {
        Entity& c = *s.find(crate);
        c.transform.position[0] = 0; c.transform.position[1] = 1.65f; c.transform.position[2] = 24;
        c.meshRenderer = MeshRenderer{};
        c.collider = Collider{};
        c.behaviours.emplace_back(createBehaviour("Destructible"));
        setField(*c.behaviours.back(), "hits", 2.f);
        setField(*c.behaviours.back(), "dropPrefab", std::string("Gem"));
    }
    Game g;
    g.start(s, meshes());
    run(g, 10);
    auto count = [&](const char* prefix) {
        int n = 0;
        for (const Entity& e : s.entities) n += e.name.rfind(prefix, 0) == 0;
        return n;
    };
    int gemsBefore = count("Gem");
    io.AddMousePosEvent(400, 300);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    run(g, 2);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK(count("Bullet") == 1);   // one shot (cooldown 0.15 s)
    run(g, 30);                    // 10 m at 40 m/s: it has arrived
    CHECK(count("Bullet") == 0);
    CHECK(s.find(crate));          // 2 hits needed
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    run(g, 2);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    run(g, 30);
    CHECK(!s.find(crate));
    CHECK(count("Gem") == gemsBefore + 1);   // dropped
    bool logged = false;
    for (const auto& l : g.log()) logged |= l.second == "Destroyed Crate";
    CHECK(logged);
}

TEST(spawner_respects_max_alive) {
    test::ImGuiContextGuard imgui;
    Scene s = sceneWithPrefab();
    EntityId sp = s.create("Spawner").id;
    s.find(sp)->behaviours.emplace_back(createBehaviour("Spawner"));
    Behaviour& b = *s.find(sp)->behaviours.back();
    setField(b, "prefab", std::string("Thing"));
    setField(b, "interval", 0.1f);
    setField(b, "maxAlive", 3.f);
    Game g;
    g.start(s, meshes());
    run(g, 60);
    int things = 0;
    for (const Entity& e : s.entities) things += e.parent == kNoEntity && e.name.rfind("Thing", 0) == 0;
    CHECK(things == 1 + 3);   // the original + maxAlive
}

TEST(prefabs_saved_with_scene_and_as_files) {
    MeshLibrary lib = buildMeshLibrary();
    auto obj = test::tempPath("tri.obj");
    test::writeFile(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    std::string err;
    int tri = importObj(lib, obj.string(), err);
    REQUIRE(tri >= 0);
    Scene s = sceneWithPrefab();
    s.prefabs[0].entities[1].meshRenderer = MeshRenderer{tri};   // a prefab-only use of an imported mesh

    auto scenePath = test::tempPath("prefabs.vkw").string();
    REQUIRE(saveScene(scenePath, s, lib, textures(), err));
    MeshLibrary lib2 = buildMeshLibrary();
    Scene loaded;
    REQUIRE(loadScene(scenePath, loaded, lib2, textures(), err));
    REQUIRE(loaded.prefabs.size() == 1);
    CHECK(loaded.prefabs[0].name == "Thing");
    CHECK(json::write(entityToJson(loaded.prefabs[0].entities[1], lib2, textures())) ==
          json::write(entityToJson(s.prefabs[0].entities[1], lib, textures())));

    auto prefabPath = test::tempPath("thing.prefab").string();
    REQUIRE(savePrefab(prefabPath, s.prefabs[0], lib, textures(), err));
    MeshLibrary lib3 = buildMeshLibrary();
    Prefab p;
    REQUIRE(loadPrefab(prefabPath, p, lib3, textures(), err));
    CHECK(p.entities.size() == 2);
    CHECK(lib3.count() == (int)MeshKind::Count + 1);   // the imported mesh came along
    CHECK(!loadPrefab(scenePath, p, lib3, textures(), err));      // a scene isn't a prefab
}
