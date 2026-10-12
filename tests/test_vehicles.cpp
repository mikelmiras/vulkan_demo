// Vehicles (Jolt wheeled vehicles) driven by VehicleDriver, recorded contacts, and the player's crouching,
// climbing and swimming
#include "test.h"

#include "game/game.h"
#include "imgui.h"
#include "scene/scene_diff.h"
#include "scene/serialization.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

void run(Game& g, int frames) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    }
}

Vec3 worldPos(const Scene& s, const char* name) {
    Mat4 m = s.worldMatrix(s.findByName(name)->id);
    return {m.m[12], m.m[13], m.m[14]};
}

Entity& box(Scene& s, const char* name, Vec3 pos, Vec3 size) {
    Entity& e = s.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    e.collider = Collider{};
    e.collider->size = size;
    return e;
}

// A 1.2 t car on a big floor: front wheels steer, back wheels drive, each shown by a child entity
Scene carScene() {
    Scene s;
    box(s, "Ground", {0, -0.5f, 0}, {400, 1, 400});
    Entity& car = box(s, "Car", {0, 1.2f, 0}, {1.8f, 0.8f, 4.f});
    car.rigidBody = RigidBody{};
    car.rigidBody->mass = 1200.f;
    Vehicle v;
    const char* names[] = {"Wheel FL", "Wheel FR", "Wheel BL", "Wheel BR"};
    for (int i = 0; i < 4; ++i) {
        Vehicle::Wheel w;
        bool front = i < 2;
        w.position = {i % 2 ? 0.9f : -0.9f, -0.2f, front ? -1.4f : 1.4f};
        w.steering = front;
        w.driven = !front;
        w.entity = names[i];
        v.wheels.push_back(w);
    }
    car.vehicle = v;
    car.behaviours.emplace_back(createBehaviour("VehicleDriver"));
    EntityId carId = car.id;
    for (const char* n : names) s.create(n, carId);
    return s;
}

}  // namespace

TEST(vehicle_drives_steers_and_brakes) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = carScene();
    Game g;
    g.start(s, meshes());
    run(g, 90);   // settle on its suspension
    Vec3 start = worldPos(s, "Car");
    CHECK(start.y > 0.3f && start.y < 1.2f);   // resting on its wheels, not on its body
    CHECK(worldPos(s, "Wheel FL").y > 0.2f);    // the wheel shown about a radius above the ground
    CHECK(worldPos(s, "Wheel FL").y < 0.7f);

    io.AddKeyEvent(ImGuiKey_W, true);
    run(g, 120);
    io.AddKeyEvent(ImGuiKey_W, false);
    const Vehicle& v = *s.findByName("Car")->vehicle;
    Vec3 driven = worldPos(s, "Car");
    CHECK(start.z - driven.z > 3.f);              // forward is -Z
    CHECK(std::fabs(driven.x - start.x) < 0.5f);  // straight
    CHECK(v.speed > 2.f);
    CHECK(v.gear >= 1);
    // The wheels roll: one has turned away from upright around its axle
    Mat4 wheel = s.worldMatrix(s.findByName("Wheel BL")->id);
    CHECK(std::fabs(transformDir(wheel, {0, 1, 0}).y) < 0.999f || std::fabs(transformDir(wheel, {0, 0, 1}).z) < 0.999f);

    // Back brakes it to a stop
    io.AddKeyEvent(ImGuiKey_S, true);
    for (int i = 0; i < 300 && v.speed > 0.3f; ++i) run(g, 1);
    io.AddKeyEvent(ImGuiKey_S, false);
    CHECK(v.speed < 0.5f);

    // Forward and right: it turns right (+X)
    Vec3 before = worldPos(s, "Car");
    io.AddKeyEvent(ImGuiKey_W, true);
    io.AddKeyEvent(ImGuiKey_D, true);
    run(g, 150);
    io.AddKeyEvent(ImGuiKey_W, false);
    io.AddKeyEvent(ImGuiKey_D, false);
    CHECK(worldPos(s, "Car").x - before.x > 1.f);

    // Taking the vehicle off leaves a plain body (no crash, still simulated)
    s.findByName("Car")->vehicle.reset();
    run(g, 30);
    CHECK(worldPos(s, "Car").y > 0.f);
    g.stop();
}

TEST(vehicle_saves_and_loads) {
    Scene s = carScene();
    MeshLibrary m = buildMeshLibrary();
    TextureLibrary t;
    json::Value doc = sceneToJson(s, m, t);
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(doc, back, m, t, error));
    const Entity* car = back.findByName("Car");
    REQUIRE(car && car->vehicle);
    CHECK(car->vehicle->wheels.size() == 4);
    CHECK(car->vehicle->wheels[2].entity == "Wheel BL");
    CHECK(car->vehicle->wheels[0].steering && !car->vehicle->wheels[0].driven);
    CHECK(sameEntity(*car, *s.findByName("Car")));
}

TEST(physics_records_contacts_when_asked) {
    test::ImGuiContextGuard imgui;
    Scene s;
    box(s, "Ground", {0, -0.5f, 0}, {20, 1, 20});
    Entity& crate = box(s, "Crate", {0, 0.6f, 0}, {1, 1, 1});
    crate.rigidBody = RigidBody{};
    Game g;
    g.start(s, meshes());
    run(g, 30);
    CHECK(g.physics().contacts().empty());   // (off unless asked)
    g.setRecordContacts(true);
    run(g, 2);
    REQUIRE(!g.physics().contacts().empty());
    for (const Physics::ContactPoint& c : g.physics().contacts()) {
        CHECK_NEAR(c.position.y, 0.0, 0.05);
        CHECK_NEAR(std::fabs(c.normal.y), 1.0, 0.01);
    }
    g.setRecordContacts(false);
    run(g, 1);
    CHECK(g.physics().contacts().empty());
}

namespace {

Scene playerScene() {
    Scene s;
    box(s, "Ground", {0, -0.5f, 0}, {100, 1, 100});
    Entity& p = s.create("Player");
    Collider body;
    body.shape = Collider::Shape::Capsule;
    body.center = {0, 0.9f, 0};
    body.height = 1.8f;
    body.radius = 0.35f;
    p.collider = body;
    p.behaviours.emplace_back(createBehaviour("PlayerController"));
    EntityId id = p.id;
    Entity& eyes = s.create("Eyes", id);
    eyes.camera = Camera{};
    eyes.transform.position[1] = 1.65f;
    return s;
}

}  // namespace

TEST(player_crouches_under_a_ceiling) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = playerScene();
    Game g;
    g.start(s, meshes());
    run(g, 10);
    io.AddKeyEvent(ImGuiKey_C, true);
    run(g, 30);
    const Entity& p = *s.findByName("Player");
    CHECK(p.collider->height < 1.1f);
    CHECK_NEAR(p.collider->center.y - p.collider->height * 0.5f, 0.0, 1e-4);   // the feet stay put
    CHECK(s.findByName("Eyes")->transform.position[1] < 1.f);
    // Crouched, it walks at half speed under a low roof
    Vec3 a = worldPos(s, "Player");
    io.AddKeyEvent(ImGuiKey_W, true);
    run(g, 60);
    io.AddKeyEvent(ImGuiKey_W, false);
    CHECK_NEAR(a.z - worldPos(s, "Player").z, 3.0, 0.3);
    Vec3 here = worldPos(s, "Player");
    box(s, "Roof", {here.x, 1.4f, here.z}, {4, 0.2f, 4});
    run(g, 2);
    io.AddKeyEvent(ImGuiKey_C, false);
    run(g, 30);
    CHECK(s.findByName("Player")->collider->height < 1.1f);   // no room: still crouched
    s.remove(s.findByName("Roof")->id);
    run(g, 30);
    CHECK_NEAR(s.findByName("Player")->collider->height, 1.8, 1e-4);
    CHECK_NEAR(s.findByName("Eyes")->transform.position[1], 1.65, 1e-3);
}

TEST(player_climbs_ladders_and_swims) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = playerScene();
    Entity& ladder = box(s, "Ladder", {0, 3, -0.6f}, {1, 6, 0.2f});
    ladder.tags.push_back("Ladder");
    Entity& pool = box(s, "Pool", {20, 2, 0}, {6, 4, 6});
    pool.collider->trigger = true;
    pool.tags.push_back("Water");
    Game g;
    g.start(s, meshes());
    run(g, 10);
    // Facing -Z into the ladder: forward climbs
    io.AddKeyEvent(ImGuiKey_W, true);
    run(g, 60);
    io.AddKeyEvent(ImGuiKey_W, false);
    CHECK(worldPos(s, "Player").y > 2.f);   // climbSpeed 3 m/s
    run(g, 30);
    CHECK(worldPos(s, "Player").y > 2.f);   // holds on: doesn't fall

    // In the pool: jump swims up, letting go sinks slowly
    Entity* p = s.findByName("Player");
    p->transform.position[0] = 20.f;
    p->transform.position[1] = 1.f;
    p->transform.position[2] = 0.f;
    run(g, 5);
    io.AddKeyEvent(ImGuiKey_Space, true);
    run(g, 30);
    io.AddKeyEvent(ImGuiKey_Space, false);
    CHECK(worldPos(s, "Player").y > 1.8f);
    run(g, 60);   // (the swim upward eases off)
    float up = worldPos(s, "Player").y;
    run(g, 30);
    float sank = up - worldPos(s, "Player").y;
    CHECK(sank > 0.1f && sank < 0.5f);   // about 0.5 m/s: much slower than falling (about 2 m in half a second)
    g.stop();
}
