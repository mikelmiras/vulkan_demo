// physics/: queries, characters, triggers and rigid bodies (all on Jolt Physics)
#include "test.h"

#include <algorithm>

#include "game/game.h"
#include "imgui.h"
#include "physics/physics.h"

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

// A character like the player: a capsule with its feet at the entity's origin
EntityId addCharacter(Scene& s, Vec3 feet) {
    Entity& e = s.create("Hero");
    e.transform.position[0] = feet.x; e.transform.position[1] = feet.y; e.transform.position[2] = feet.z;
    Collider c;
    c.shape = Collider::Shape::Capsule;
    c.center = {0, 0.9f, 0};
    c.radius = 0.35f;
    c.height = 1.8f;
    e.collider = c;
    return e.id;
}

// Moves the character repeatedly at `velocity`, like a controller would (stepping the world too); returns its feet
Vec3 walk(Physics& p, Scene& s, EntityId id, Vec3 velocity, int steps, bool* grounded = nullptr) {
    for (int i = 0; i < steps; ++i) {
        CharacterMove m = p.moveCharacter(s, meshes(), *s.find(id), velocity * (1.f / 60.f), 1.f / 60.f);
        p.step(s, meshes(), 1.f / 60.f);
        if (grounded) *grounded = m.grounded;
    }
    return s.find(id)->transform.pos();
}

}  // namespace

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
    Physics w;
    w.step(s, meshes(), 0.f);

    RaycastHit hit;
    REQUIRE(w.raycast({0, 0, 0}, {0, 0, -1}, 100.f, hit));
    CHECK(hit.entity == s.findByName("Box")->id);
    CHECK_NEAR(hit.distance, 9.f, 1e-3);
    CHECK_VEC(hit.normal, test::v3(0, 0, 1), 1e-4f);
    REQUIRE(w.raycast({0, 0, 0}, {3, 0, 0}, 100.f, hit));   // direction needn't be unit length
    CHECK_NEAR(hit.distance, 4.5f, 1e-3);
    CHECK_VEC(hit.normal, test::v3(-1, 0, 0), 1e-3f);
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
    Physics w;
    w.step(s, meshes(), 0.f);
    CHECK(w.overlapSphere({0.9f, 0, 0}, 0.5f).size() == 1);
    CHECK(w.overlapSphere({1.5f, 0, 0}, 1.1f).size() == 2);
    CHECK(w.overlapSphere({1.5f, 0, 0}, 1.1f, false).size() == 1);   // without triggers
    CHECK(w.overlapSphere({10, 0, 0}, 1.f).empty());
    RaycastHit hit;
    CHECK(!w.raycast({3, 5, 0}, {0, -1, 0}, 10.f, hit));             // rays go through triggers
}

TEST(physics_character_stands_and_slides) {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {100, 1, 100});
    box(s, "Wall", {0, 1, -3}, {10, 2, 0.5f});
    EntityId hero = addCharacter(s, {0, 0.5f, 0});
    Physics w;
    w.step(s, meshes(), 0.f);

    bool grounded = false;
    Vec3 feet = walk(w, s, hero, {0, -5, 0}, 60, &grounded);   // falls onto the floor and stays
    CHECK(grounded);
    CHECK_NEAR(feet.y, 0.f, 0.03);
    CHECK(w.isCharacter(hero));
    feet = walk(w, s, hero, {0, -1, 0}, 1, &grounded);   // resting: still grounded
    CHECK(grounded);

    // Walk into the wall (front face at z = -2.75): stopped at the radius (plus a little padding)
    Vec3 stopped = walk(w, s, hero, {0, -1, -6}, 120);
    CHECK_NEAR(stopped.z, -2.75f + 0.35f, 0.05);
    // Diagonally into it: blocked in z, slides along x
    Vec3 slid = walk(w, s, hero, {4, -1, -4}, 30);
    CHECK_NEAR(slid.z, stopped.z, 0.05);
    CHECK(slid.x > stopped.x + 1.5f);
    // Rays find the character
    RaycastHit hit;
    REQUIRE(w.raycast(slid + Vec3{0, 1, 5}, {0, 0, -1}, 10.f, hit));
    CHECK(hit.entity == hero);
}

TEST(physics_fast_move_does_not_tunnel) {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {100, 1, 100});
    box(s, "Thin wall", {0, 1, -5}, {10, 4, 0.05f});
    EntityId hero = addCharacter(s, {0, 0.02f, 0});
    Physics w;
    w.step(s, meshes(), 0.f);
    CharacterMove m = w.moveCharacter(s, meshes(), *s.find(hero), {0, 0, -20}, 1.f / 60.f);   // 20 m in one step
    CHECK(m.hitWall);
    CHECK(s.find(hero)->transform.position[2] > -5.f);
}

TEST(physics_ceiling_steps_and_crates) {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {100, 1, 100});
    box(s, "Ceiling", {0, 2.5f, 0}, {4, 1, 4});
    box(s, "Crate", {0, 0.25f, -5}, {2, 0.5f, 2});
    box(s, "Step", {5, 0.15f, 0}, {2, 0.3f, 4});
    EntityId hero = addCharacter(s, {0, 0.02f, 0});
    Physics w;
    w.step(s, meshes(), 0.f);
    CharacterMove m = w.moveCharacter(s, meshes(), *s.find(hero), {0, 0.5f, 0}, 1.f / 60.f);   // head into the ceiling
    CHECK(m.hitCeiling);
    CHECK(s.find(hero)->transform.position[1] < 0.3f);
    s.find(hero)->transform.position[1] = 1.5f;   // teleported over the crate: the character follows
    s.find(hero)->transform.position[2] = -5.f;
    bool grounded = false;
    Vec3 feet = walk(w, s, hero, {0, -5, 0}, 60, &grounded);   // dropped onto the crate
    CHECK(grounded);
    CHECK_NEAR(feet.y, 0.5f, 0.03);
    // Back on the floor, walk at a 30 cm step: up it, without jumping
    s.find(hero)->transform.position[0] = 2.5f;
    s.find(hero)->transform.position[1] = 0.02f;
    s.find(hero)->transform.position[2] = 0.f;
    feet = walk(w, s, hero, {3, -1, 0}, 60);
    CHECK(feet.x > 4.5f);
    CHECK_NEAR(feet.y, 0.3f, 0.05);
}

// Standing on a platform game code moves (kinematic): the character goes with it
TEST(physics_character_rides_platform) {
    Scene s;
    EntityId platform = box(s, "Platform", {0, -0.25f, 0}, {4, 0.5f, 4}).id;
    EntityId hero = addCharacter(s, {0, 0.02f, 0});
    Physics w;
    w.step(s, meshes(), 0.f);
    walk(w, s, hero, {0, -5, 0}, 20);   // settle
    for (int i = 0; i < 60; ++i) {      // the platform slides 2 m along x over a second
        s.find(platform)->transform.position[0] += 2.f / 60.f;
        walk(w, s, hero, {0, -1, 0}, 1);
    }
    CHECK(s.find(hero)->transform.position[0] > 1.5f);
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

// ---------------------------------------------------------------------------------------------
// Rigid bodies
// ---------------------------------------------------------------------------------------------
namespace {

Scene floorScene() {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {100, 1, 100});
    return s;
}

EntityId addBody(Scene& s, const char* name, Vec3 pos, Collider::Shape shape, float bounciness = 0.f) {
    EntityId id = s.create(name).id;
    Entity& e = *s.find(id);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    Collider c;
    c.shape = shape;
    e.collider = c;   // unit box / radius 0.5 sphere
    RigidBody rb;
    rb.bounciness = bounciness;
    e.rigidBody = rb;
    return id;
}

void simulate(Physics& bodies, Scene& s, float seconds) {
    for (int i = 0; i < (int)(seconds * 60.f); ++i) bodies.step(s, meshes(), 1.f / 60.f);
}

Vec3 pos(const Scene& s, EntityId id) { return s.find(id)->transform.pos(); }

}  // namespace

TEST(rigid_body_falls_and_rests) {
    Scene s = floorScene();
    Physics bodies;
    EntityId b = addBody(s, "Box", {0, 5, 0}, Collider::Shape::Box);
    simulate(bodies, s, 0.25f);
    CHECK(pos(s, b).y < 5.f - 0.4f);   // ~ g t^2 / 2 = 0.56
    simulate(bodies, s, 3.f);
    CHECK_NEAR(pos(s, b).y, 0.5f, 0.025);   // resting on the floor (within the contact slop)
    CHECK(length(s.find(b)->rigidBody->velocity) < 1e-2f);
    CHECK(bodies.bodyCount() == 2);
}

TEST(rigid_body_bounces_and_settles) {
    Scene s = floorScene();
    Physics bodies;
    EntityId ball = addBody(s, "Ball", {0, 4, 0}, Collider::Shape::Sphere, 0.8f);
    float lowest = 10.f, peakAfterBounce = 0.f;
    bool bounced = false;
    for (int i = 0; i < 240; ++i) {
        bodies.step(s, meshes(), 1.f / 60.f);
        float y = pos(s, ball).y;
        if (!bounced) {
            lowest = std::min(lowest, y);
            bounced = s.find(ball)->rigidBody->velocity.y > 0.f;
        } else {
            peakAfterBounce = std::max(peakAfterBounce, y);
        }
    }
    CHECK(lowest > 0.45f);             // never sank into the floor
    CHECK(peakAfterBounce > 2.f);      // came back up (0.8 restitution from 3.5 m)
    simulate(bodies, s, 10.f);
    CHECK_NEAR(pos(s, ball).y, 0.5f, 0.03);   // eventually at rest
}

TEST(rigid_body_stack_is_stable) {
    Scene s = floorScene();
    EntityId a = addBody(s, "A", {0, 0.5f, 0}, Collider::Shape::Box);
    EntityId b = addBody(s, "B", {0, 1.5f, 0}, Collider::Shape::Box);
    EntityId c = addBody(s, "C", {0, 2.5f, 0}, Collider::Shape::Box);
    Physics bodies;
    simulate(bodies, s, 5.f);
    CHECK_NEAR(pos(s, a).y, 0.5f, 0.03);
    CHECK_NEAR(pos(s, b).y, 1.5f, 0.05);
    CHECK_NEAR(pos(s, c).y, 2.5f, 0.07);
    CHECK(std::fabs(pos(s, c).x) < 0.01f);
}

TEST(rigid_body_momentum_exchange) {
    Scene s;   // no gravity, no floor: a moving ball hits a resting one of the same mass head-on
    EntityId moving = addBody(s, "Moving", {0, 0, 0}, Collider::Shape::Sphere, 1.f);
    EntityId resting = addBody(s, "Resting", {3, 0, 0}, Collider::Shape::Sphere, 1.f);
    for (EntityId id : {moving, resting}) {
        s.find(id)->rigidBody->useGravity = false;
        s.find(id)->rigidBody->damping = 0.f;
    }
    s.find(moving)->rigidBody->velocity = {5, 0, 0};
    Physics bodies;
    simulate(bodies, s, 1.f);
    CHECK_NEAR(s.find(moving)->rigidBody->velocity.x, 0.f, 0.3);   // elastic, equal masses: velocities swap
    CHECK_NEAR(s.find(resting)->rigidBody->velocity.x, 5.f, 0.3);
}

TEST(rigid_body_pushed_by_player_and_shot) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = buildDefaultScene();
    // A tall crate right in front of the player (tall enough to be in the line of fire at eye height)
    EntityId crate = s.create("Test crate").id;
    {
        Entity& c = *s.find(crate);
        c.transform.position[0] = 0; c.transform.position[1] = 1.5f; c.transform.position[2] = 32;
        c.transform.scale[1] = 3.f;
        c.meshRenderer = MeshRenderer{};
        c.collider = Collider{};
        c.rigidBody = RigidBody{};
        c.rigidBody->lockRotation = true;   // pushed and shot, not tipped over (a free one would topple onto its side)
    }
    Game g;
    g.start(s, meshes());
    auto frame = [&](int n) {
        for (int i = 0; i < n; ++i) {
            io.DeltaTime = 1.f / 60.f;
            ImGui::NewFrame();
            g.frame(1.f / 60.f, true, true);
            ImGui::EndFrame();
        }
    };
    frame(20);
    float z0 = pos(s, crate).z;
    io.AddKeyEvent(ImGuiKey_W, true);
    frame(60);
    io.AddKeyEvent(ImGuiKey_W, false);
    float z1 = pos(s, crate).z;
    CHECK(z1 < z0 - 2.f);   // walked into it: pushed ahead (towards -Z)
    frame(60);              // slides to a stop
    float z2 = pos(s, crate).z;
    io.AddMousePosEvent(400, 300);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);   // shoot it
    frame(2);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(30);
    CHECK(pos(s, crate).z < z2 - 0.3f);   // knocked further
}

// Bodies turn: a box dropped onto an edge tumbles flat; one with lockRotation never turns
TEST(rigid_body_tumbles_and_lock_rotation) {
    for (bool locked : {false, true}) {
        Scene s = floorScene();
        EntityId b = addBody(s, "Tilted", {0, 2, 0}, Collider::Shape::Box);
        Entity& e = *s.find(b);
        e.transform.rotation[2] = 40.f;   // lands on an edge
        e.rigidBody->lockRotation = locked;
        Physics bodies;
        simulate(bodies, s, 4.f);
        Mat4 m = s.worldMatrix(b);
        Vec3 up = normalize(transformDir(m, {0, 1, 0}));
        if (locked) {
            CHECK_NEAR(s.find(b)->transform.rotation[2], 40.0, 0.5);   // still tilted (resting on its edge)
        } else {
            // Settled on a face: one of its axes points straight up
            Vec3 x = normalize(transformDir(m, {1, 0, 0}));
            CHECK(std::fabs(up.y) > 0.99f || std::fabs(x.y) > 0.99f);
            CHECK_NEAR(pos(s, b).y, 0.5f, 0.03);
        }
    }
}

// A body removed from the scene (or losing its collider) leaves the simulation
TEST(rigid_body_removed) {
    Scene s = floorScene();
    EntityId b = addBody(s, "Box", {0, 3, 0}, Collider::Shape::Box);
    Physics bodies;
    simulate(bodies, s, 0.1f);
    CHECK(bodies.bodyCount() == 2);
    s.remove(b);
    simulate(bodies, s, 0.1f);
    CHECK(bodies.bodyCount() == 1);
}

// Queries over the whole world find exactly what testing every body on its own would: rays and sphere overlaps
// against a random field, compared with worlds of one body each
TEST(physics_queries_match_single_bodies) {
    Scene s;
    uint32_t seed = 12345;
    auto rnd = [&](float a, float b) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return a + (b - a) * ((seed & 0xFFFFFF) / float(0x1000000));
    };
    for (int i = 0; i < 100; ++i) {
        Entity& e = box(s, "b", test::v3(rnd(-20, 20), rnd(0, 6), rnd(-20, 20)), test::v3(rnd(0.3f, 3), rnd(0.3f, 3), rnd(0.3f, 3)));
        e.transform.rotation[1] = rnd(0, 90);
        if (i % 3 == 1) { e.collider->shape = Collider::Shape::Sphere; e.collider->radius = 0.5f; }
        if (i % 3 == 2) { e.collider->shape = Collider::Shape::Capsule; e.collider->radius = 0.3f; e.collider->height = 1.5f; }
        if (i % 7 == 0) e.collider->trigger = true;
    }
    struct Probe { Vec3 o, d; float r; };
    std::vector<Probe> probes;
    for (int r = 0; r < 200; ++r)
        probes.push_back({test::v3(rnd(-22, 22), rnd(0.5f, 6), rnd(-22, 22)), normalize(test::v3(rnd(-1, 1), rnd(-0.3f, 0.3f), rnd(-1, 1))), rnd(0.2f, 4.f)});

    Physics all;
    all.step(s, meshes(), 0.f);
    std::vector<float> want(probes.size(), -1.f);
    std::vector<std::vector<EntityId>> wantNear(probes.size());
    for (const Entity& e : s.entities) {   // one body at a time
        Scene one;
        one.add(e);
        Physics w;
        w.step(one, meshes(), 0.f);
        for (size_t k = 0; k < probes.size(); ++k) {
            RaycastHit h;
            if (w.raycast(probes[k].o, probes[k].d, 50.f, h) && (want[k] < 0.f || h.distance < want[k])) want[k] = h.distance;
            for (EntityId id : w.overlapSphere(probes[k].o, probes[k].r)) wantNear[k].push_back(id);
        }
    }
    int hits = 0, overlaps = 0;
    for (size_t k = 0; k < probes.size(); ++k) {
        RaycastHit got;
        bool found = all.raycast(probes[k].o, probes[k].d, 50.f, got);
        CHECK(found == (want[k] >= 0.f));
        if (found && want[k] >= 0.f) {
            CHECK_NEAR(got.distance, want[k], 1e-3);
            ++hits;
        }
        std::vector<EntityId> near = all.overlapSphere(probes[k].o, probes[k].r);
        std::sort(near.begin(), near.end());
        std::sort(wantNear[k].begin(), wantNear[k].end());
        CHECK(near == wantNear[k]);
        overlaps += (int)near.size();
    }
    CHECK(hits > 10);
    CHECK(overlaps > 10);
}
