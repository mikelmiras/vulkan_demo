// game/: fixed-step loop, input actions, behaviours
#include "test.h"

#include "game/game.h"
#include "imgui.h"

namespace {

// Advances the game by `frames` rendered frames of `dt` seconds, with an ImGui frame around each
void run(Game& g, int frames, float dt = 1.f / 60.f) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = dt;
        ImGui::NewFrame();
        g.frame(dt, true, true);
        ImGui::EndFrame();
    }
}

Vec3 worldPos(const Scene& s, const char* name) {
    const Entity* e = s.findByName(name);
    Mat4 m = s.worldMatrix(e ? e->id : kNoEntity);
    return {m.m[12], m.m[13], m.m[14]};
}

void setString(Behaviour& b, const char* field, const std::string& value) {
    struct S : FieldVisitor {
        const char* f;
        std::string v;
        void field(const char*, float&, float, float, float) override {}
        void field(const char*, bool&) override {}
        void field(const char*, Vec3&, float) override {}
        void field(const char* n, std::string& s) override { if (std::string(n) == f) s = v; }
    } s;
    s.f = field;
    s.v = value;
    b.fields(s);
}

}  // namespace

TEST(game_fixed_timestep_is_frame_rate_independent) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Game g;
    g.start(s);
    run(g, 10, 1.f / 30.f);
    CHECK(g.tick() == 20);
    run(g, 40, 1.f / 240.f);   // 40 frames at 240 fps = 1/6 s = 10 updates
    CHECK(g.tick() == 30);
    unsigned long long t0 = g.tick();
    run(g, 1, 2.f);            // a 2 s hitch is capped
    CHECK(g.tick() - t0 == (unsigned long long)Game::kMaxStepsPerFrame);
}

TEST(game_pause_and_step) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Game g;
    g.start(s);
    g.setPaused(true);
    run(g, 30);
    CHECK(g.tick() == 0);
    g.step();
    CHECK(g.tick() == 1);
}

TEST(input_short_tap_is_seen_exactly_once) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s;
    Game g;
    g.start(s);
    run(g, 2);
    io.AddKeyEvent(ImGuiKey_Space, true);
    run(g, 1, 1.f / 240.f);   // too short for a fixed update
    io.AddKeyEvent(ImGuiKey_Space, false);
    CHECK(g.input().pressed("jump"));   // latched
    int seen = 0;
    for (int i = 0; i < 8; ++i) {
        unsigned long long t0 = g.tick();
        bool p = g.input().pressed("jump");
        run(g, 1, 1.f / 240.f);
        if (p && g.tick() > t0) ++seen;
    }
    CHECK(seen == 1);
}

TEST(input_disabled_devices_read_released) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    InputMap in;
    io.AddKeyEvent(ImGuiKey_W, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    ImGui::NewFrame();
    in.beginFrame(false, true);
    CHECK(!in.down("move_forward"));
    CHECK(in.down("look"));
    in.beginFrame(true, false);
    CHECK(in.down("move_forward"));
    CHECK(!in.down("look"));
    CHECK(in.axis("move_back", "move_forward") == 1.f);
    ImGui::EndFrame();
}

TEST(behaviours_registry_and_set_field) {
    for (const char* name : {"Spin", "Bob", "Patrol", "LookAt", "PlayerController", "Pickup"})
        CHECK(createBehaviour(name) != nullptr);
    CHECK(createBehaviour("Nope") == nullptr);
    auto spin = createBehaviour("Spin");
    CHECK(setField(*spin, "speed", 45.f));
    CHECK(!setField(*spin, "nope", 1.f));
}

TEST(behaviours_play_copy_is_independent) {
    test::ImGuiContextGuard imgui;
    Scene edit = buildDefaultScene();
    Scene play = edit;   // what the editor does on Play
    Game g;
    g.start(play);
    Vec3 before = worldPos(edit, "Centre Torus");
    run(g, 60);
    CHECK_VEC(worldPos(edit, "Centre Torus"), before, 0.f);
    CHECK(std::fabs(worldPos(play, "Centre Torus").y - before.y) > 0.01f);
    // Spin at 1.5 rad/s for 1 s
    CHECK_NEAR(play.findByName("Pillar Top.001")->transform.rotation[1], 1.5f / DEG2RAD, 0.5);
}

TEST(behaviours_player_walks_jumps_and_looks) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = buildDefaultScene();
    Game g;
    g.start(s);
    Vec3 p0 = worldPos(s, "Player");
    io.AddKeyEvent(ImGuiKey_W, true);
    run(g, 60);
    io.AddKeyEvent(ImGuiKey_W, false);
    Vec3 p1 = worldPos(s, "Player");
    CHECK_NEAR(length(p1 - p0), 6.f, 0.3f);   // moveSpeed 6 for 1 s
    CHECK(p1.z < p0.z);                        // forward is -Z

    io.AddKeyEvent(ImGuiKey_Space, true);
    run(g, 1);
    io.AddKeyEvent(ImGuiKey_Space, false);
    float peak = 0.f;
    for (int i = 0; i < 90; ++i) {
        run(g, 1);
        peak = std::max(peak, worldPos(s, "Player").y);
    }
    CHECK(peak > 2.5f);
    CHECK_NEAR(worldPos(s, "Player").y, 1.7f, 1e-3);   // landed at eye height

    io.AddMousePosEvent(100, 300);
    run(g, 1);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    run(g, 1);
    io.AddMousePosEvent(700, 300);   // 600 px * 0.15 deg/px = 90 deg to the right
    run(g, 2);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    run(g, 1);
    Mat4 m = s.worldMatrix(s.findByName("Player")->id);
    CHECK_VEC(normalize(transformDir(m, {0, 0, -1})), test::v3(1, 0, 0), 1e-3f);
}

TEST(behaviours_pickup_destroys_and_logs) {
    test::ImGuiContextGuard imgui;
    Scene s = buildDefaultScene();
    Game g;
    g.start(s);
    size_t before = s.entities.size();
    Vec3 gem = worldPos(s, "Gem.003");
    Entity* player = s.findByName("Player");
    player->transform.position[0] = gem.x;
    player->transform.position[2] = gem.z;
    run(g, 2);
    CHECK(!s.findByName("Gem.003"));
    CHECK(s.entities.size() == before - 1);
    REQUIRE(!g.log().empty());
    CHECK(g.log().back().second == "Collected Gem.003 (7 left)");
}

TEST(behaviours_patrol_and_look_at) {
    test::ImGuiContextGuard imgui;
    Scene s;
    s.create("Walker").behaviours.emplace_back(createBehaviour("Patrol"));   // offset (0, 0, 6) at 3 u/s
    Entity& watcher = s.create("Watcher");
    watcher.transform.position[0] = 10;
    auto look = createBehaviour("LookAt");
    setString(*look, "target", "Walker");
    watcher.behaviours.emplace_back(std::move(look));
    Game g;
    g.start(s);
    run(g, 60);
    Vec3 walker = worldPos(s, "Walker");
    CHECK_NEAR(walker.z, 3.f, 0.1f);
    Mat4 wm = s.worldMatrix(s.findByName("Watcher")->id);
    CHECK_VEC(normalize(transformDir(wm, {0, 0, -1})), normalize(walker - worldPos(s, "Watcher")), 1e-3f);
    run(g, 180);   // 4 s total = 12 units on a 6-unit leg: back at the start
    CHECK_NEAR(worldPos(s, "Walker").z, 0.f, 0.1f);
}
