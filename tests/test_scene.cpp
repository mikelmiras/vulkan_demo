// scene/: hierarchy, entity operations, serialization
#include "test.h"

#include <fstream>

#include "core/json.h"
#include "scene/scene.h"
#include "scene/serialization.h"

namespace {

Vec3 worldPos(const Scene& s, EntityId id) {
    Mat4 m = s.worldMatrix(id);
    return {m.m[12], m.m[13], m.m[14]};
}

// A rotated, scaled parent A with child B at local (0, 0, 1)
struct TwoLevel {
    Scene s;
    EntityId a, b;
    TwoLevel() {
        a = s.create("A").id;
        Transform& t = s.find(a)->transform;
        t.position[0] = 10;
        t.rotation[1] = 90;
        t.scale[0] = t.scale[1] = t.scale[2] = 2;
        b = s.create("B", a).id;
        s.find(b)->transform.position[2] = 1;
    }
};

}  // namespace

TEST(scene_world_matrices_follow_parents) {
    TwoLevel t;
    CHECK_VEC(worldPos(t.s, t.b), test::v3(12, 0, 0), 1e-4f);   // +Z rotated 90 deg about Y -> +X, scaled by 2
    std::vector<Mat4> all = t.s.worldMatrices();
    REQUIRE(all.size() == 2);
    CHECK_VEC(test::v3(all[1].m[12], all[1].m[13], all[1].m[14]), worldPos(t.s, t.b), 1e-5f);
}

TEST(scene_reparent_keeps_world_position) {
    TwoLevel t;
    Vec3 before = worldPos(t.s, t.b);
    REQUIRE(t.s.setParent(t.b, kNoEntity, true));
    CHECK_VEC(worldPos(t.s, t.b), before, 1e-4f);
    CHECK_NEAR(t.s.find(t.b)->transform.scale[0], 2.f, 1e-4);
    REQUIRE(t.s.setParent(t.b, t.a, true));
    CHECK_VEC(worldPos(t.s, t.b), before, 1e-4f);
    CHECK_VEC(t.s.find(t.b)->transform.pos(), test::v3(0, 0, 1), 1e-4f);
}

TEST(scene_reparent_refuses_cycles) {
    TwoLevel t;
    CHECK(!t.s.setParent(t.a, t.b, true));
    CHECK(!t.s.setParent(t.a, t.a, true));
    CHECK(!t.s.setParent(t.a, 12345, true));
    CHECK(t.s.find(t.a)->parent == kNoEntity);
}

TEST(scene_duplicate_and_remove_subtrees) {
    TwoLevel t;
    EntityId c = t.s.create("C", t.b).id;
    EntityId copy = t.s.duplicate(t.a);
    CHECK(t.s.entities.size() == 6);
    CHECK(t.s.find(copy)->name == "A.001");
    std::vector<EntityId> sub = t.s.subtree(copy);
    REQUIRE(sub.size() == 3);
    for (EntityId id : sub) CHECK(id != t.a && id != t.b && id != c);
    CHECK_VEC(worldPos(t.s, sub[1]), worldPos(t.s, t.b), 1e-5f);   // same pose as the original
    t.s.remove(t.a);
    CHECK(t.s.entities.size() == 3);
    CHECK(!t.s.find(t.b) && !t.s.find(c));
}

TEST(scene_visibility_is_inherited) {
    TwoLevel t;
    CHECK(t.s.visibleInHierarchy(t.b));
    t.s.find(t.a)->visible = false;
    CHECK(!t.s.visibleInHierarchy(t.b));
}

TEST(scene_unique_names) {
    Scene s;
    s.create("Cube.001");
    s.create("Cube.002");
    CHECK(s.uniqueName("Cube.001") == "Cube.003");
    CHECK(s.uniqueName("Sphere") == "Sphere");
    CHECK(s.uniqueName("Cube") == "Cube");
}

TEST(scene_default_scene_is_sane) {
    Scene s = buildDefaultScene();
    REQUIRE(!s.entities.empty());
    const Entity* player = s.findByName("Player");
    const Entity* eyes = s.findByName("Player Camera");
    REQUIRE(player && eyes);
    CHECK(player->collider && player->collider->shape == Collider::Shape::Capsule);
    CHECK(eyes->parent == player->id);
    const Entity* firstCamera = nullptr;   // the one the game renders through
    for (const Entity& e : s.entities)
        if (e.camera) { firstCamera = &e; break; }
    CHECK(firstCamera == eyes);
    for (const Entity& e : s.entities) CHECK(e.parent == kNoEntity || s.find(e.parent));
}

// ---------------------------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------------------------
TEST(serialization_json_roundtrip) {
    MeshLibrary lib = buildMeshLibrary();
    Scene s = buildDefaultScene();
    std::string err;
    auto path = test::tempPath("roundtrip.vkw").string();
    REQUIRE(saveScene(path, s, lib, textures(), err));
    Scene loaded;
    MeshLibrary lib2 = buildMeshLibrary();
    REQUIRE(loadScene(path, loaded, lib2, textures(), err));
    REQUIRE(loaded.entities.size() == s.entities.size());
    for (size_t i = 0; i < s.entities.size(); ++i)   // every component and behaviour field, compared as JSON
        CHECK(json::write(entityToJson(s.entities[i], lib, textures())) == json::write(entityToJson(loaded.entities[i], lib2, textures())));
}

TEST(serialization_imported_meshes_are_reimported) {
    MeshLibrary lib = buildMeshLibrary();
    auto obj = test::tempPath("tri.obj");
    test::writeFile(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    std::string err;
    int id = importObj(lib, obj.string(), err);
    REQUIRE(id >= 0);
    Scene s;
    s.create("T").meshRenderer = MeshRenderer{id};
    auto path = test::tempPath("imported.vkw").string();
    REQUIRE(saveScene(path, s, lib, textures(), err));
    MeshLibrary fresh = buildMeshLibrary();
    Scene loaded;
    REQUIRE(loadScene(path, loaded, fresh, textures(), err));
    REQUIRE(loaded.entities[0].meshRenderer);
    CHECK(fresh.count() == (int)MeshKind::Count + 1);
    CHECK(fresh.name(loaded.entities[0].meshRenderer->mesh) == std::string("tri"));
}

TEST(serialization_loads_legacy_text_format) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("old.vkw");
    test::writeFile(path, "# vkworld scene v2\n# comment\n"
                          "\"Ground\" \"Plane\" 0 0 0 0 0 0 600 1 600 0.42 0.55 0.38 1 1 1 0 0 0\n"
                          "\"Cam\" \"None\" 0 7 34 -9 0 0 1 1 1 1 1 1 0 1 0 0 0 0 camera 1.0472\n"
                          "\"Spinner\" Cube 1 2 3 0 0 0 1 1 1 0.5 0.5 0.5 0 1 0 1.5 0.5 0.2\n");
    Scene s;
    std::string err;
    REQUIRE(loadScene(path.string(), s, lib, textures(), err));
    REQUIRE(s.entities.size() == 3);
    CHECK(s.entities[0].meshRenderer && s.entities[0].meshRenderer->checker);
    CHECK(s.entities[1].camera && !s.entities[1].meshRenderer);
    REQUIRE(s.entities[2].behaviours.size() == 2);   // spin + bob -> Spin and Bob behaviours
    CHECK(std::string(s.entities[2].behaviours[0]->typeName()) == "Spin");
}

TEST(serialization_converts_animator_component) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("anim.vkw");
    test::writeFile(path, R"({"format":"vkworld-scene","version":3,"entities":[
        {"id":1,"name":"T","animator":{"spin":0.8,"bob":0.4,"phase":0}}]})");
    Scene s;
    std::string err;
    REQUIRE(loadScene(path.string(), s, lib, textures(), err));
    REQUIRE(s.entities[0].behaviours.size() == 2);
    CHECK(std::string(s.entities[0].behaviours[1]->typeName()) == "Bob");
}

TEST(serialization_repairs_broken_hierarchy) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("broken.vkw");
    test::writeFile(path, R"({"format":"vkworld-scene","version":3,"entities":[
        {"id":1,"name":"X","parent":2},{"id":2,"name":"Y","parent":1},{"id":3,"name":"Z","parent":99},
        {"name":"no id"}]})");
    Scene s;
    std::string err;
    REQUIRE(loadScene(path.string(), s, lib, textures(), err));
    REQUIRE(s.entities.size() == 4);
    CHECK(s.entities[2].parent == kNoEntity);                   // missing parent -> root
    CHECK(!(s.isAncestor(1, 2) && s.isAncestor(2, 1)));         // cycle cut
    CHECK(s.entities[3].id != kNoEntity);                       // id-less entity got one
}

TEST(serialization_rejects_bad_files) {
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    std::string err;
    auto garbage = test::tempPath("garbage.vkw");
    test::writeFile(garbage, "{\"format\": \"vkworld-scene\", \"entities\": [1, 2,");
    CHECK(!loadScene(garbage.string(), s, lib, textures(), err));
    auto typeless = test::tempPath("typeless.vkw");
    test::writeFile(typeless, R"({"format":"vkworld-scene","version":3,"entities":[{"id":1,"name":"T","behaviours":[{"speed":1}]}]})");
    CHECK(!loadScene(typeless.string(), s, lib, textures(), err));
    auto future = test::tempPath("future.vkw");
    test::writeFile(future, R"({"format":"vkworld-scene","version":999,"entities":[]})");
    CHECK(!loadScene(future.string(), s, lib, textures(), err));
    CHECK(!loadScene(test::tempPath("missing.vkw").string(), s, lib, textures(), err));
}

// Scene::find keeps an id index; `entities` is public, so it must survive edits made behind its back
TEST(scene_find_survives_direct_edits) {
    Scene s;
    EntityId a = s.create("A").id, b = s.create("B").id, c = s.create("C", a).id;
    REQUIRE(s.find(c) && s.find(c)->name == "C");
    s.entities.erase(s.entities.begin());   // A gone without remove(): indices shift
    CHECK(!s.find(a));
    REQUIRE(s.find(b) && s.find(b)->name == "B");
    REQUIRE(s.find(c) && s.find(c)->name == "C");
    std::swap(s.entities[0], s.entities[1]);
    CHECK(s.find(b)->name == "B");
    CHECK(s.find(c)->name == "C");
    Entity d;
    d.id = 77;
    d.name = "D";
    s.entities.push_back(d);   // added directly, not through add()
    REQUIRE(s.find(77) && s.find(77)->name == "D");
    s.entities.clear();
    CHECK(!s.find(b));
}

TEST(scene_remove_many_with_descendants) {
    Scene s;
    EntityId root = s.create("Root").id;
    EntityId child = s.create("Child", root).id;
    EntityId other = s.create("Other").id;
    EntityId grandchild = s.create("Grandchild", child).id;
    EntityId late = s.create("Late").id;
    s.setParent(other, grandchild);   // a child listed before its parent: needs more than one pass
    EntityId keep = s.create("Keep").id;
    s.remove(std::vector<EntityId>{late, root});
    CHECK(s.entities.size() == 1);
    CHECK(s.find(keep));
    for (EntityId gone : {root, child, other, grandchild, late}) CHECK(!s.find(gone));
}

// A behaviour type nobody registered (its game module isn't loaded) loads as a placeholder and saves back unchanged
TEST(serialization_keeps_unknown_behaviours) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("unknown.vkw");
    test::writeFile(path, R"({"format":"vkworld-scene","version":3,"entities":[{"id":1,"name":"T","behaviours":[
        {"type":"Nope","speed":2.5,"target":"Gem","flags":[1,2]},{"type":"Spin","speed":45}]}]})");
    Scene s;
    std::string err;
    REQUIRE(loadScene(path.string(), s, lib, textures(), err));
    REQUIRE(s.entities.size() == 1 && s.entities[0].behaviours.size() == 2);
    CHECK(std::string(s.entities[0].behaviours[0]->typeName()) == "Nope");
    CHECK(dynamic_cast<MissingBehaviour*>(s.entities[0].behaviours[0].get()) != nullptr);
    CHECK(countMissingBehaviours(s) == 1);

    auto again = test::tempPath("unknown_again.vkw");
    REQUIRE(saveScene(again.string(), s, lib, textures(), err));
    Scene s2;
    REQUIRE(loadScene(again.string(), s2, lib, textures(), err));
    json::Value saved = behaviourToJson(*s2.entities[0].behaviours[0]);
    CHECK(saved["type"].asString() == "Nope");
    CHECK_NEAR(saved["speed"].asFloat(0.f), 2.5, 1e-6);
    CHECK(saved["target"].asString() == "Gem");
    CHECK(saved["flags"].elements().size() == 2);

    // Detaching a registered type keeps its fields; reattaching brings the real behaviour back
    CHECK(detachBehaviours(s2, {"Spin"}) == 1);
    CHECK(countMissingBehaviours(s2) == 2);
    CHECK(reattachBehaviours(s2) == 1);   // Spin is registered, Nope still isn't
    REQUIRE(dynamic_cast<MissingBehaviour*>(s2.entities[0].behaviours[1].get()) == nullptr);
    CHECK_NEAR(behaviourToJson(*s2.entities[0].behaviours[1])["speed"].asFloat(0.f), 45.0, 1e-4);
}

namespace {

struct Typed : BehaviourBase<Typed> {
    static constexpr const char* kTypeName = "TestTyped";
    int count = 3;
    int mode = 1;
    std::string target = "Player", prefab, sound;
    void fields(FieldVisitor& v) override {
        v.field("count", count, 0, 10);
        v.field("mode", mode, {"slow", "fast", "warp"});
        v.field("target", target, FieldRef::Entity);
        v.field("prefab", prefab, FieldRef::Prefab);
        v.field("sound", sound, FieldRef::Sound);
    }
};

json::Value parseJson(const char* text) {
    json::Value v;
    std::string error;
    json::parse(text, v, error);
    return v;
}

}  // namespace

// Ints, choices (saved by name) and references survive saving; old values and visitors still work
TEST(behaviour_typed_fields) {
    registerBehaviour<Typed>("test");
    Typed t;
    t.count = 7;
    t.mode = 2;
    t.target = "Boss";
    t.prefab = "Bullet";
    json::Value v = behaviourToJson(t);
    CHECK(v["count"].asInt() == 7);
    CHECK(v["mode"].asString() == "warp");
    CHECK(v["target"].asString() == "Boss");
    std::unique_ptr<Behaviour> back = behaviourFromJson(v);
    REQUIRE(back);
    auto& b = static_cast<Typed&>(*back);
    CHECK(b.count == 7 && b.mode == 2 && b.target == "Boss" && b.prefab == "Bullet");

    // A float saved before the field was an int is rounded; an unknown choice keeps the default; a number works
    json::Value old = parseJson(R"({"type": "TestTyped", "count": 4.6, "mode": "ludicrous"})");
    std::unique_ptr<Behaviour> oldB = behaviourFromJson(old), numB = behaviourFromJson(parseJson(R"({"type": "TestTyped", "mode": 0})"));
    auto& o = static_cast<Typed&>(*oldB);
    CHECK(o.count == 5);
    CHECK(o.mode == 1);
    auto& n = static_cast<Typed&>(*numB);
    CHECK(n.mode == 0);

    // setField reaches them
    CHECK(setField(b, "count", 2.f) && b.count == 2);
    CHECK(setField(b, "mode", std::string("slow")) && b.mode == 0);
    CHECK(setField(b, "target", std::string("Hero")) && b.target == "Hero");
    CHECK(!setField(b, "mode", std::string("nope")));

    // A visitor that only knows the basic kinds sees an int as a float and a reference as a string
    struct Basic : FieldVisitor {
        std::vector<std::string> seen;
        void field(const char* name, float& v, float, float, float max) override {
            seen.push_back(std::string(name) + "=" + std::to_string((int)v) + "/" + std::to_string((int)max));
            v += 1.f;
        }
        void field(const char*, bool&) override {}
        void field(const char*, Vec3&, float) override {}
        void field(const char* name, std::string& s) override { seen.push_back(std::string(name) + ":" + s); }
    } basic;
    b.fields(basic);
    REQUIRE(basic.seen.size() == 5);
    CHECK(basic.seen[0] == "count=2/10");
    CHECK(basic.seen[1] == "mode=0/2");
    CHECK(basic.seen[2] == "target:Hero");
    CHECK(b.count == 3 && b.mode == 1);   // written back
}
