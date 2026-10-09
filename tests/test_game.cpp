// game/: fixed-step loop, input actions, behaviours
#include "test.h"

#include "game/game.h"
#include "scene/serialization.h"
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

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
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
    g.start(s, meshes());
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
    g.start(s, meshes());
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
    g.start(s, meshes());
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
    in.beginFrame(false, true, ImVec2(0, 0));
    CHECK(!in.down("move_forward"));
    CHECK(in.down("look"));
    in.beginFrame(true, false, ImVec2(0, 0));
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
    g.start(play, meshes());
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
    g.start(s, meshes());
    run(g, 30);   // settle onto the ground (starts 5 cm above it)
    Vec3 p0 = worldPos(s, "Player");
    CHECK_NEAR(p0.y, 0.f, 0.01);
    io.AddKeyEvent(ImGuiKey_W, true);
    run(g, 60);
    io.AddKeyEvent(ImGuiKey_W, false);
    Vec3 p1 = worldPos(s, "Player");
    CHECK_NEAR(length(p1 - p0), 6.f, 0.3f);   // moveSpeed 6 for 1 s
    CHECK(p1.z < p0.z);                        // forward is -Z
    CHECK_NEAR(p1.y, 0.f, 0.01);               // still on the ground

    io.AddKeyEvent(ImGuiKey_Space, true);
    run(g, 1);
    io.AddKeyEvent(ImGuiKey_Space, false);
    float peak = 0.f;
    for (int i = 0; i < 90; ++i) {
        run(g, 1);
        peak = std::max(peak, worldPos(s, "Player").y);
    }
    CHECK(peak > 1.f);                               // v^2 / 2g = 6.5^2 / 36 = 1.17
    CHECK_NEAR(worldPos(s, "Player").y, 0.f, 0.01);  // landed

    io.AddMousePosEvent(100, 300);
    run(g, 1);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    run(g, 1);
    io.AddMousePosEvent(700, 300);   // 600 px * 0.15 deg/px = 90 deg to the right
    run(g, 2);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    run(g, 1);
    Mat4 cam = s.worldMatrix(s.findByName("Player Camera")->id);
    CHECK_VEC(normalize(transformDir(cam, {0, 0, -1})), test::v3(1, 0, 0), 1e-3f);
    Mat4 body = s.worldMatrix(s.findByName("Player")->id);
    CHECK_VEC(normalize(transformDir(body, {0, 1, 0})), test::v3(0, 1, 0), 1e-4f);   // the capsule stays upright
}

TEST(behaviours_pickup_destroys_and_logs) {
    test::ImGuiContextGuard imgui;
    Scene s = buildDefaultScene();
    Game g;
    g.start(s, meshes());
    auto gems = [&] {   // (not all entities: the demo's spawner adds balls)
        int n = 0;
        for (const Entity& e : s.entities) n += e.name.rfind("Gem.", 0) == 0;
        return n;
    };
    int before = gems();
    Vec3 gem = worldPos(s, "Gem.003");
    Entity* player = s.findByName("Player");
    player->transform.position[0] = gem.x;
    player->transform.position[2] = gem.z;
    run(g, 2);   // the gem's trigger sphere now overlaps the player's capsule
    CHECK(!s.findByName("Gem.003"));
    CHECK(gems() == before - 1);
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
    g.start(s, meshes());
    run(g, 60);
    Vec3 walker = worldPos(s, "Walker");
    CHECK_NEAR(walker.z, 3.f, 0.1f);
    Mat4 wm = s.worldMatrix(s.findByName("Watcher")->id);
    CHECK_VEC(normalize(transformDir(wm, {0, 0, -1})), normalize(walker - worldPos(s, "Watcher")), 1e-3f);
    run(g, 180);   // 4 s total = 12 units on a 6-unit leg: back at the start
    CHECK_NEAR(worldPos(s, "Walker").z, 0.f, 0.1f);
}

TEST(input_key_names_roundtrip) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        ImGuiKey key = (ImGuiKey)k;
        if (keyFromName(keyName(key)) != key) {
            test::fail(__FILE__, __LINE__, std::string("key name doesn't round-trip: ") + keyName(key));
            break;
        }
    }
    CHECK(keyFromName("NotAKey") == ImGuiKey_None);
    CHECK(isGamepadKey(ImGuiKey_GamepadFaceDown) && !isGamepadKey(ImGuiKey_W));
}

TEST(input_bindings_saved_with_scene) {
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    s.inputActions = {{"shoot", {ImGuiKey_F, ImGuiKey_GamepadR2}, ImGuiMouseButton_Left}, {"duck", {ImGuiKey_C}, -1}};
    std::string err;
    auto path = test::tempPath("bindings.vkw").string();
    REQUIRE(saveScene(path, s, lib, textures(), err));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, lib, textures(), err));
    REQUIRE(loaded.inputActions.size() == 2);
    CHECK(loaded.inputActions[0].name == "shoot");
    CHECK(loaded.inputActions[0].keys == s.inputActions[0].keys);
    CHECK(loaded.inputActions[0].mouseButton == ImGuiMouseButton_Left);
    CHECK(loaded.inputActions[1].mouseButton == -1);
    // Files without bindings get the defaults
    test::writeFile(test::tempPath("nobindings.vkw"), "{\"format\":\"vkworld-scene\",\"version\":4,\"entities\":[]}");
    REQUIRE(loadScene(test::tempPath("nobindings.vkw").string(), loaded, lib, textures(), err));
    CHECK(loaded.inputActions.size() == defaultInputActions().size());
}

TEST(input_analog_sticks_and_captured_mouse) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    Scene s = buildDefaultScene();
    Game g;
    g.start(s, meshes());
    run(g, 30);   // settle
    Vec3 p0 = worldPos(s, "Player");
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   // what a backend sets when a pad is connected
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, true, 0.5f);   // half-pushed stick: half speed
    run(g, 1);
    CHECK_NEAR(g.input().value("move_forward"), 0.5f, 1e-6);
    CHECK_NEAR(g.input().axis("move_back", "move_forward"), 0.5f, 1e-6);
    run(g, 59);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.f);
    run(g, 1);
    CHECK_NEAR(length(worldPos(s, "Player") - p0), 3.f, 0.3f);

    // Right stick turns at stickLookSpeed (150 deg/s): full right for 0.6 s = 90 deg
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickRight, true, 1.f);
    run(g, 36);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickRight, false, 0.f);
    run(g, 1);
    Mat4 cam = s.worldMatrix(s.findByName("Player Camera")->id);
    CHECK_VEC(normalize(transformDir(cam, {0, 0, -1})), test::v3(1, 0, 0), 0.03f);

    // Captured mouse: movement turns the view without holding the look button
    g.input().setMouseCaptured(true);
    for (int i = 0; i < 2; ++i) {
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true, ImVec2(300.f, 0.f));   // 600 px * 0.15 deg/px = another 90 deg
        ImGui::EndFrame();
    }
    run(g, 1);
    cam = s.worldMatrix(s.findByName("Player Camera")->id);
    CHECK_VEC(normalize(transformDir(cam, {0, 0, -1})), test::v3(0, 0, 1), 0.03f);
}
