// physics/: contact maths, queries, character movement, triggers
#include "test.h"

#include "game/game.h"
#include "imgui.h"
#include "physics/collision.h"
#include "physics/physics_world.h"

using namespace collision;

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

Entity& box(Scene& s, const char* name, Vec3 pos, Vec3 size) {
    Entity& e = s.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    e.transform.scale[0] = size.x; e.transform.scale[1] = size.y; e.transform.scale[2] = size.z;
    e.collider = Collider{};   // unit box, scaled
    return e;
}

Capsule character(Vec3 feet) { return {feet + Vec3{0, 0.35f, 0}, feet + Vec3{0, 1.45f, 0}, 0.35f}; }

// Moves a character capsule repeatedly, like a controller would; returns the feet position
Vec3 walk(const PhysicsWorld& w, Vec3 feet, Vec3 velocity, int steps, bool* grounded = nullptr) {
    for (int i = 0; i < steps; ++i) {
        CharacterMove m = w.moveCapsule(character(feet), velocity * (1.f / 60.f), kNoEntity);
        feet += m.displacement;
        if (grounded) *grounded = m.grounded;
    }
    return feet;
}

}  // namespace

TEST(collision_closest_points) {
    CHECK_VEC(closestPointOnSegment({5, 1, 0}, {0, 0, 0}, {2, 0, 0}), test::v3(2, 0, 0), 1e-6f);
    CHECK_VEC(closestPointOnTriangle({0.2f, 5, 0.2f}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}), test::v3(0.2f, 0, 0.2f), 1e-6f);
    CHECK_VEC(closestPointOnTriangle({-1, 0, -1}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}), test::v3(0, 0, 0), 1e-6f);
    Obb b;
    b.center = {0, 0, 0};
    CHECK_VEC(closestPointOnObb({3, 0.2f, -9}, b), test::v3(0.5f, 0.2f, -0.5f), 1e-6f);
}

TEST(collision_sphere_contacts) {
    Contact c;
    Obb b;
    CHECK(sphereVsObb({{0, 0.8f, 0}, 0.5f}, b, c));
    CHECK_VEC(c.normal, test::v3(0, 1, 0), 1e-5f);
    CHECK_NEAR(c.depth, 0.2f, 1e-5);
    CHECK(!sphereVsObb({{0, 1.1f, 0}, 0.5f}, b, c));
    CHECK(sphereVsObb({{0.1f, 0.4f, 0}, 0.2f}, b, c));   // centre inside: out through the nearest face
    CHECK_VEC(c.normal, test::v3(0, 1, 0), 1e-5f);
    CHECK_NEAR(c.depth, 0.3f, 1e-5);
    CHECK(sphereVsSphere({{1, 0, 0}, 1}, {{0, 0, 0}, 0.5f}, c));
    CHECK_NEAR(c.depth, 0.5f, 1e-5);
    CHECK(sphereVsTriangle({{0.2f, 0.1f, 0.2f}, 0.3f}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, c));
    CHECK_NEAR(c.depth, 0.2f, 1e-5);
}

TEST(collision_obb_vs_obb) {
    Obb a, b;
    b.center = {0.9f, 0, 0};
    CHECK(obbVsObb(a, b));
    b.center = {1.1f, 0, 0};
    CHECK(!obbVsObb(a, b));
    float c = std::cos(PI / 4), s = std::sin(PI / 4);   // rotated 45 deg about Y: reaches 0.707 along X
    b.axis[0] = {c, 0, -s};
    b.axis[2] = {s, 0, c};
    b.center = {1.15f, 0, 0};
    CHECK(obbVsObb(a, b));
    b.center = {1.25f, 0, 0};
    CHECK(!obbVsObb(a, b));
}

TEST(physics_raycast_shapes_and_normals) {
    Scene s;
    box(s, "Box", {0, 0, -10}, {2, 2, 2});
    Entity& ball = s.create("Ball");
    ball.transform.position[0] = 5;
    Collider sphere;
    sphere.shape = Collider::Shape::Sphere;
    ball.collider = sphere;
    Entity& torus = s.create("Torus");
    torus.transform.position[0] = -5;
    torus.transform.scale[0] = torus.transform.scale[1] = torus.transform.scale[2] = 4;
    torus.meshRenderer = MeshRenderer{(int)MeshKind::Torus};
    torus.collider = fitCollider(meshes(), (int)MeshKind::Torus);
    PhysicsWorld w;
    w.build(s, meshes());

    RaycastHit hit;
    REQUIRE(w.raycast({0, 0, 0}, {0, 0, -1}, 100.f, hit));
    CHECK(hit.entity == s.findByName("Box")->id);
    CHECK_NEAR(hit.distance, 9.f, 1e-4);
    CHECK_VEC(hit.normal, test::v3(0, 0, 1), 1e-5f);
    REQUIRE(w.raycast({0, 0, 0}, {3, 0, 0}, 100.f, hit));   // direction needn't be unit length
    CHECK_NEAR(hit.distance, 4.5f, 1e-4);
    CHECK_VEC(hit.normal, test::v3(-1, 0, 0), 1e-4f);
    REQUIRE(w.raycast({-5, 5, 1.4f}, {0, -1, 0}, 100.f, hit));   // down onto the torus ring (R 0.35*4, r 0.15*4)
    CHECK(hit.entity == s.findByName("Torus")->id);
    CHECK_NEAR(hit.point.y, 0.6f, 0.05);
    CHECK(hit.normal.y > 0.9f);
    CHECK(!w.raycast({-5, 5, 0}, {0, -1, 0}, 100.f, hit));        // through the hole
    CHECK(!w.raycast({0, 0, 0}, {0, 0, -1}, 5.f, hit));           // too short
    CHECK(!w.raycast({0, 0, 0}, {0, 0, -1}, 100.f, hit, s.findByName("Box")->id));   // ignored
}

TEST(physics_overlap_sphere) {
    Scene s;
    box(s, "A", {0, 0, 0}, {1, 1, 1});
    box(s, "B", {3, 0, 0}, {1, 1, 1}).collider->trigger = true;
    PhysicsWorld w;
    w.build(s, meshes());
    CHECK(w.overlapSphere({0.9f, 0, 0}, 0.5f).size() == 1);
    CHECK(w.overlapSphere({1.5f, 0, 0}, 1.1f).size() == 2);
    CHECK(w.overlapSphere({1.5f, 0, 0}, 1.1f, false).size() == 1);   // without triggers
    CHECK(w.overlapSphere({10, 0, 0}, 1.f).empty());
}

TEST(physics_character_stands_and_slides) {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {100, 1, 100});
    box(s, "Wall", {0, 1, -3}, {10, 2, 0.5f});
    PhysicsWorld w;
    w.build(s, meshes());

    bool grounded = false;
    Vec3 feet = walk(w, {0, 0.5f, 0}, {0, -5, 0}, 60, &grounded);   // falls onto the floor and stays
    CHECK(grounded);
    CHECK_NEAR(feet.y, 0.f, 0.01);
    feet = walk(w, feet, {0, -1, 0}, 1, &grounded);   // resting: still grounded
    CHECK(grounded);

    // Walk into the wall (front face at z = -2.75): stopped at the radius
    Vec3 stopped = walk(w, feet, {0, -1, -6}, 120);
    CHECK_NEAR(stopped.z, -2.75f + 0.35f, 0.02);
    // Diagonally into it: blocked in z, slides along x
    Vec3 slid = walk(w, stopped, {4, -1, -4}, 30);
    CHECK_NEAR(slid.z, stopped.z, 0.02);
    CHECK(slid.x > stopped.x + 1.5f);
}

TEST(physics_fast_move_does_not_tunnel) {
    Scene s;
    box(s, "Thin wall", {0, 1, -5}, {10, 4, 0.05f});
    PhysicsWorld w;
    w.build(s, meshes());
    CharacterMove m = w.moveCapsule(character({0, 0.2f, 0}), {0, 0, -20}, kNoEntity);   // 20 m in one step
    CHECK(m.hitWall);
    CHECK(m.displacement.z > -5.f);
}

TEST(physics_ceiling_and_step_on_box) {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {100, 1, 100});
    box(s, "Ceiling", {0, 2.5f, 0}, {4, 1, 4});
    box(s, "Crate", {0, 0.25f, -5}, {2, 0.5f, 2});
    PhysicsWorld w;
    w.build(s, meshes());
    CharacterMove m = w.moveCapsule(character({0, 0, 0}), {0, 0.5f, 0}, kNoEntity);   // head into the ceiling
    CHECK(m.hitCeiling);
    CHECK(m.displacement.y < 0.5f);
    bool grounded = false;
    Vec3 feet = walk(w, {0, 1.5f, -5}, {0, -5, 0}, 60, &grounded);   // dropped onto the crate
    CHECK(grounded);
    CHECK_NEAR(feet.y, 0.5f, 0.01);
}

namespace {
struct Counter : BehaviourBase<Counter> {
    static constexpr const char* kTypeName = "TestCounter";
    int enters = 0, exits = 0;
    void onTriggerEnter(GameContext&, Entity&, Entity&) override { ++enters; }
    void onTriggerExit(GameContext&, Entity&, Entity&) override { ++exits; }
};
}  // namespace

TEST(physics_trigger_enter_and_exit_once) {
    Scene s;
    // (creating an entity can reallocate the list: keep ids, not references, across creations)
    EntityId zoneId = box(s, "Zone", {0, 0, 0}, {2, 2, 2}).id;
    s.find(zoneId)->collider->trigger = true;
    s.find(zoneId)->behaviours.emplace_back(std::make_unique<Counter>());
    EntityId moverId = box(s, "Mover", {5, 0, 0}, {1, 1, 1}).id;
    s.find(moverId)->behaviours.emplace_back(std::make_unique<Counter>());
    Game g;
    g.start(s, meshes());
    auto counter = [&](EntityId id) { return s.find(id)->behaviour<Counter>(); };
    auto stepTo = [&](float x, int frames) {
        s.find(moverId)->transform.position[0] = x;
        for (int i = 0; i < frames; ++i) g.step();
    };
    stepTo(5, 3);
    CHECK(counter(zoneId)->enters == 0);
    stepTo(1, 5);   // overlapping for 5 steps: one enter, on both sides
    CHECK(counter(zoneId)->enters == 1);
    CHECK(counter(moverId)->enters == 1);
    stepTo(5, 3);
    CHECK(counter(zoneId)->exits == 1);
    CHECK(counter(moverId)->exits == 1);
    stepTo(0, 1);
    CHECK(counter(zoneId)->enters == 2);
}
