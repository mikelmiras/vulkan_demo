// Prefabs, spawning, and the behaviours built on them
#include "test.h"

#include "core/json.h"
#include "game/game.h"
#include "imgui.h"
#include "scene/prefab_sync.h"
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
    s.addPrefab(s.makePrefab(root, "Thing"));
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

// ---------------------------------------------------------------------------------------------
// Linked instances
// ---------------------------------------------------------------------------------------------
namespace {

// Prefab "Lamp": a red root with a child "Bulb" at y = 1; two instances placed at x = 0 and x = 10
struct TwoInstances {
    Scene s;
    MeshLibrary meshes = buildMeshLibrary();
    uint32_t prefab = 0;
    EntityId a = kNoEntity, b = kNoEntity;
    TwoInstances() {
        EntityId root = s.create("Lamp").id;
        MeshRenderer red;
        red.color = {1, 0, 0};
        s.find(root)->meshRenderer = red;
        EntityId bulb = s.create("Bulb", root).id;
        s.find(bulb)->transform.position[1] = 1.f;
        prefab = s.addPrefab(s.makePrefab(root, "Lamp")).id;
        s.remove(root);
        a = place(0.f);
        b = place(10.f);
    }
    EntityId place(float x) {
        std::vector<Entity> copies = s.instantiate(*s.findPrefab(prefab));
        copies[0].transform.position[0] = x;
        EntityId id = copies[0].id;
        for (Entity& e : copies) s.add(std::move(e));
        return id;
    }
    Entity* child(EntityId root, const char* name) {
        for (EntityId id : s.subtree(root))
            if (s.find(id)->name == name) return s.find(id);
        return nullptr;
    }
};

}  // namespace

TEST(prefab_instances_are_linked) {
    TwoInstances t;
    const Entity* bulb = t.child(t.a, "Bulb");
    REQUIRE(bulb && bulb->prefab && t.s.find(t.a)->prefab);
    CHECK(t.s.find(t.a)->prefab->prefab == t.prefab);
    CHECK(instanceRoot(t.s, bulb->id) == t.a);
    CHECK(instanceRoot(t.s, t.a) == t.a);
    EntityId bulbId = bulb->id;
    EntityId plain = t.s.create("Plain").id;   // (may move the entities: `bulb` is stale from here)
    CHECK(instanceRoot(t.s, plain) == kNoEntity);
    CHECK(instanceOverrides(t.s, bulbId, t.meshes, textures()).empty());
    std::vector<std::string> rootOverrides = instanceOverrides(t.s, t.a, t.meshes, textures());
    CHECK(rootOverrides.empty());   // x = 0: placed where the prefab has it
    CHECK(instanceOverrides(t.s, t.b, t.meshes, textures()) == std::vector<std::string>{"transform.position"});
}

TEST(prefab_apply_updates_other_instances_keeping_overrides) {
    TwoInstances t;
    t.s.find(t.b)->meshRenderer->color = {0, 0, 1};   // B overrides its colour
    CHECK(instanceOverrides(t.s, t.b, t.meshes, textures()).size() == 2);   // position + colour

    // Edit instance A: green, bulb higher, a new child; apply it to the prefab
    t.s.find(t.a)->meshRenderer->color = {0, 1, 0};
    t.child(t.a, "Bulb")->transform.position[1] = 2.f;
    t.s.create("Shade", t.a);
    int updated = applyToPrefab(t.s, t.a, t.prefab, t.meshes, textures());
    CHECK(updated == 1);
    const Prefab& p = *t.s.findPrefab(t.prefab);
    CHECK(p.entities.size() == 3);
    CHECK_VEC(p.entities[0].meshRenderer->color, test::v3(0, 1, 0), 1e-6f);
    CHECK(p.entities[0].transform.position[0] == 0.f);

    const Entity& b = *t.s.find(t.b);
    CHECK_VEC(b.meshRenderer->color, test::v3(0, 0, 1), 1e-6f);   // override kept
    CHECK(b.transform.position[0] == 10.f);                        // placement kept
    REQUIRE(t.child(t.b, "Bulb"));
    CHECK(t.child(t.b, "Bulb")->transform.position[1] == 2.f);    // followed the prefab
    REQUIRE(t.child(t.b, "Shade"));                                // new child appeared
    CHECK(t.child(t.b, "Shade")->prefab.has_value());
    CHECK(t.child(t.a, "Shade")->prefab.has_value());              // and A's new child is now linked too

    // Removing a child and applying removes it everywhere
    t.s.remove(t.child(t.a, "Bulb")->id);
    applyToPrefab(t.s, t.a, t.prefab, t.meshes, textures());
    CHECK(!t.child(t.b, "Bulb"));
    CHECK(t.child(t.b, "Shade"));
}

TEST(prefab_revert_and_unlink) {
    TwoInstances t;
    Entity& b = *t.s.find(t.b);
    b.meshRenderer->color = {0, 0, 1};
    b.name = "My lamp";
    t.s.remove(t.child(t.b, "Bulb")->id);
    revertInstance(t.s, t.b, t.meshes, textures());
    const Entity& rb = *t.s.find(t.b);
    CHECK_VEC(rb.meshRenderer->color, test::v3(1, 0, 0), 1e-6f);
    CHECK(rb.transform.position[0] == 10.f);   // stays where it was put
    CHECK(rb.name == "My lamp");               // and keeps its name
    CHECK(t.child(t.b, "Bulb"));                // deleted child restored

    unlinkInstance(t.s, t.b);
    for (EntityId id : t.s.subtree(t.b)) CHECK(!t.s.find(id)->prefab);
    CHECK(t.s.find(t.a)->prefab);   // the other instance is untouched
}

TEST(prefab_apply_from_unrelated_entity_keeps_instances) {
    TwoInstances t;
    EntityId other = t.s.create("Other").id;
    MeshRenderer m;
    m.color = {1, 1, 0};
    t.s.find(other)->meshRenderer = m;
    applyToPrefab(t.s, other, t.prefab, t.meshes, textures());
    REQUIRE(t.s.find(t.a) && t.s.find(t.b));                         // instances survive (matched by root)
    CHECK_VEC(t.s.find(t.b)->meshRenderer->color, test::v3(1, 1, 0), 1e-6f);
    CHECK(!t.child(t.b, "Bulb"));                                    // the new prefab has no bulb
    CHECK(t.s.find(other)->prefab.has_value());                       // "Other" became an instance
}

TEST(prefab_links_saved_and_survive_rename) {
    TwoInstances t;
    t.s.findPrefab(t.prefab)->name = "Renamed";
    std::string err;
    auto path = test::tempPath("links.vkw").string();
    REQUIRE(saveScene(path, t.s, t.meshes, textures(), err));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, t.meshes, textures(), err));
    REQUIRE(loaded.findPrefab(t.prefab));
    CHECK(loaded.findPrefab(t.prefab)->name == "Renamed");
    CHECK(instanceRoot(loaded, t.b) == t.b);
    // Editing the loaded prefab still reaches the instances
    loaded.find(t.a)->meshRenderer->color = {0, 1, 1};
    CHECK(applyToPrefab(loaded, t.a, t.prefab, t.meshes, textures()) == 1);
    CHECK_VEC(loaded.find(t.b)->meshRenderer->color, test::v3(0, 1, 1), 1e-6f);
}
