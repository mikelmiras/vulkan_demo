// Joints (Jolt constraints) between rigid bodies, breaking, motors, kinematic bodies, ragdolls from skeletons
#include "test.h"

#include "assets/gltf.h"
#include "game/game.h"
#include "game/ragdoll.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

void steps(Game& g, int n) {
    for (int i = 0; i < n; ++i) {
        ImGui::NewFrame();
        g.step();
        ImGui::EndFrame();
    }
}

Entity& body(Scene& s, const char* name, Vec3 pos, Vec3 size = {0.5f, 0.5f, 0.5f}) {
    Entity& e = s.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    e.transform.scale[0] = size.x; e.transform.scale[1] = size.y; e.transform.scale[2] = size.z;
    e.collider = Collider{};
    e.rigidBody = RigidBody{};
    e.rigidBody->damping = 0.f;
    return e;
}

Vec3 pos(const Scene& s, const char* name) {
    Mat4 m = s.worldMatrix(s.findByName(name)->id);
    return {m.m[12], m.m[13], m.m[14]};
}

}  // namespace

// A pendulum (ball joint to the world) keeps its length; a fixed pair falls as one; a slider stays on its axis
TEST(joints_ball_fixed_slider) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& bob = body(s, "Bob", {0, 8, 0});
    bob.rigidBody->velocity = {4, 0, 0};
    Joint ball;
    ball.type = Joint::Type::Ball;
    ball.anchor = {0, 4, 0};   // local: scaled by 0.5, so 2 m above it, at (0, 10, 0)
    bob.joint = ball;
    body(s, "A", {10, 10, 0});
    Entity& b = body(s, "B", {11, 10, 0});
    Joint fixed;
    fixed.type = Joint::Type::Fixed;
    fixed.connectedTo = "A";
    b.joint = fixed;
    Entity& slide = body(s, "Slide", {-10, 10, 0});
    Joint slider;
    slider.type = Joint::Type::Slider;
    slider.axis = {1, 0, 0};
    slider.minLimit = -1.f;
    slider.maxLimit = 1.f;
    slide.joint = slider;
    slide.rigidBody->velocity = {5, 0, 0};
    Game g;
    g.start(s, meshes());
    for (int i = 0; i < 6; ++i) {
        steps(g, 20);
        CHECK_NEAR(length(pos(s, "Bob") - Vec3{0, 10, 0}), 2.0, 0.02);
        CHECK_NEAR(length(pos(s, "B") - pos(s, "A")), 1.0, 0.01);
        CHECK_NEAR(pos(s, "Slide").y, 10.0, 0.01);
        CHECK(pos(s, "Slide").x < -10.f + 1.f + 0.02f);
    }
    CHECK(pos(s, "A").y < 0.f);    // the pair fell (nothing holds it)
    CHECK(pos(s, "Bob").y < 9.f);  // it swung down
    CHECK_NEAR(pos(s, "Slide").x, -9.0, 0.02);   // stopped by its limit
}

// A door on a hinge with a motor turns at its speed; a joint pulled too hard breaks
TEST(joints_hinge_motor_and_breaking) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& door = body(s, "Door", {0.5f, 1, 0}, {1, 2, 0.1f});
    door.rigidBody->useGravity = false;
    Joint hinge;
    hinge.type = Joint::Type::Hinge;
    hinge.anchor = {-0.5f, 0, 0};   // its left edge
    hinge.axis = {0, 1, 0};
    hinge.motorSpeed = 90.f;
    hinge.motorStrength = 1000.f;
    door.joint = hinge;
    Entity& weak = body(s, "Weak", {5, 5, 0}, {1, 1, 1});
    weak.rigidBody->mass = 50.f;
    Joint fixed;
    fixed.type = Joint::Type::Fixed;
    fixed.breakForce = 100.f;   // less than its weight (~900 N)
    weak.joint = fixed;
    Entity& strong = body(s, "Strong", {8, 5, 0}, {1, 1, 1});
    strong.rigidBody->mass = 50.f;
    fixed.breakForce = 5000.f;
    strong.joint = fixed;
    Game g;
    g.start(s, meshes());
    steps(g, 60);   // 1 s at 90 deg/s
    const Entity& d = *s.findByName("Door");
    Vec3 x = normalize(transformDir(s.worldMatrix(d.id), {1, 0, 0}));
    float angle = std::atan2(-x.z, x.x) / DEG2RAD;
    CHECK_NEAR(angle, 90.0, 6.0);
    Vec3 edge = transformPoint(s.worldMatrix(d.id), {-0.5f, 0, 0});
    CHECK_VEC(edge, test::v3(0, 1, 0), 0.02f);   // turning around its edge
    CHECK(s.findByName("Weak")->joint->broken);
    CHECK(pos(s, "Weak").y < 0.f);
    CHECK(!s.findByName("Strong")->joint->broken);
    CHECK_NEAR(pos(s, "Strong").y, 5.0, 0.02);
}

// A chain of parented bodies (each hinged to its parent) is written back parents first
TEST(joints_parented_chain_and_kinematic) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& top = body(s, "Top", {0, 10, 0});
    top.rigidBody->kinematic = true;   // held in place, moved by code
    EntityId parent = top.id;
    const char* names[] = {"L1", "L2", "L3"};
    for (const char* n : names) {
        Entity& link = s.create(n);
        link.parent = parent;
        link.transform.position[1] = -2.f;   // 1 m below (the parent's scale is 0.5) ... and so on
        link.collider = Collider{};
        link.rigidBody = RigidBody{};
        Joint j;
        j.type = Joint::Type::Ball;
        j.connectedTo = s.find(parent)->name;
        j.anchor = {0, 1, 0};
        link.joint = j;
        parent = link.id;
    }
    s.findByName("L1")->rigidBody->velocity = {3, 0, 0};
    Game g;
    g.start(s, meshes());
    for (int i = 0; i < 5; ++i) {
        steps(g, 15);
        Vec3 a = pos(s, "Top"), l1 = pos(s, "L1"), l2 = pos(s, "L2"), l3 = pos(s, "L3");
        CHECK_NEAR(length(l1 - a), 1.0, 0.03);
        CHECK_NEAR(length(l2 - l1), 1.0, 0.03);
        CHECK_NEAR(length(l3 - l2), 1.0, 0.03);
    }
    CHECK_VEC(pos(s, "Top"), test::v3(0, 10, 0), 1e-4f);   // kinematic: gravity doesn't move it
    s.findByName("Top")->transform.position[0] = 3.f;      // code does
    steps(g, 30);
    CHECK_NEAR(pos(s, "Top").x, 3.0, 1e-4);
    CHECK(pos(s, "L1").x > 1.f);   // dragged along

    // Joints and kinematic bodies are saved with the scene
    std::string error, path = test::tempPath("joints.vkw").string();
    REQUIRE(saveScene(path, s, meshes(), textures(), error));
    Scene loaded;
    MeshLibrary lib = buildMeshLibrary();
    REQUIRE(loadScene(path, loaded, lib, textures(), error));
    REQUIRE(loaded.findByName("L2")->joint);
    CHECK(loaded.findByName("L2")->joint->type == Joint::Type::Ball);
    CHECK(loaded.findByName("L2")->joint->connectedTo == "L1");
    CHECK_VEC(loaded.findByName("L2")->joint->anchor, test::v3(0, 1, 0), 1e-6f);
    CHECK(loaded.findByName("Top")->rigidBody->kinematic);
}

#ifdef VKWORLD_TEST_ASSETS
// A ragdoll from the bending bar's two bones: animated until it goes limp, then it falls in one piece
TEST(joints_ragdoll) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    Prefab p;
    std::string error, warning;
    REQUIRE(importGltfPrefab(std::string(VKWORLD_TEST_ASSETS) + "/bending_bar.gltf", lib, textures(), p, error, warning));
    for (Entity& e : s.instantiate(p)) s.add(std::move(e));
    Entity& floor = s.create("Floor");
    floor.transform.position[1] = -3.f;
    floor.transform.scale[0] = floor.transform.scale[2] = 20.f;
    floor.collider = Collider{};
    EntityId root = s.findByName("bending_bar")->id;
    REQUIRE(makeRagdoll(s, root) == 2);
    const Entity* b0 = s.findByName("Bone0");
    const Entity* b1 = s.findByName("Bone1");
    REQUIRE(b0->rigidBody && b1->rigidBody && b0->collider);
    CHECK(b0->rigidBody->kinematic);
    CHECK(!b0->joint);
    REQUIRE(b1->joint);
    CHECK(b1->joint->connectedTo == "Bone0");
    CHECK(b1->joint->type == Joint::Type::Cone);

    Game g;
    g.start(s, lib);
    steps(g, 30);   // animated: the bend at 0.5 s puts bone 1 at (-1, 0, 0)
    CHECK_VEC(pos(s, "Bone1"), test::v3(-1, 0, 0), 1e-2f);
    setRagdollLimp(s, root, true);
    CHECK(!s.find(root)->animator->playing);
    for (int i = 0; i < 6; ++i) {
        steps(g, 20);
        CHECK_NEAR(length(pos(s, "Bone1") - pos(s, "Bone0")), 1.0, 0.05);   // still in one piece
    }
    CHECK(pos(s, "Bone0").y < -1.5f);   // fell to the floor
    CHECK(pos(s, "Bone0").y > -3.5f);   // and not through it
}
#endif
