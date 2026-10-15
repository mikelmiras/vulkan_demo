// game/: fixed-step loop, input actions, behaviours
#include "test.h"

#include <fstream>
#include <iterator>

#include "core/project.h"
#include "game/game.h"
#include "game/strings.h"
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

namespace {

struct Mover : BehaviourBase<Mover> {
    static constexpr const char* kTypeName = "TestMover";
    void update(GameContext&, Entity& self) override { self.transform.position[0] += 1.f; }
};

}  // namespace

// Drawn between the last two fixed updates, by how far the frame is into the next one
TEST(game_render_interpolation) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity e;
    e.name = "Mover";
    e.behaviours.emplace_back(std::make_unique<Mover>());
    Entity child;   // follows its parent, interpolated as well
    child.name = "Child";
    child.transform.position[1] = 2.f;
    EntityId parent = s.add(std::move(e)).id;
    child.parent = parent;
    s.add(std::move(child));
    Game g;
    g.start(s, meshes());
    std::vector<Mat4> w = g.worldMatrices();
    CHECK_NEAR(w[0].m[12], 0.0, 1e-6);   // nothing to blend from yet
    run(g, 1, Game::kFixedDt * 1.5f);    // one update (x 0 -> 1), half way to the next
    w = g.worldMatrices();
    CHECK_NEAR(w[0].m[12], 0.5, 1e-4);
    CHECK_NEAR(w[1].m[12], 0.5, 1e-4);
    CHECK_NEAR(w[1].m[13], 2.0, 1e-4);
    CHECK_NEAR(worldPos(s, "Mover").x, 1.0, 1e-6);   // the scene itself is where the update left it
    g.setInterpolation(false);
    CHECK_NEAR(g.worldMatrices()[0].m[12], 1.0, 1e-6);
    g.setInterpolation(true);
    g.setPaused(true);   // paused: the latest state, not a blend
    CHECK_NEAR(g.worldMatrices()[0].m[12], 1.0, 1e-6);
}

// String tables: nested keys, English under the language, fallbacks, {@key} in text, switching languages
TEST(strings_tables_and_translation) {
    StringTable t;
    REQUIRE(t.loadJson(R"({"menu": {"resume": "Reanudar"}, "hud.score": "Puntos", "title": "¡Hola, señor!"})",
                       R"({"menu": {"resume": "Resume", "quit": "Quit"}, "hud.score": "Score"})"));
    CHECK(t.get("menu.resume") == "Reanudar");
    CHECK(t.get("menu.quit") == "Quit");                 // English fills the gap
    CHECK(t.get("missing", "Fallback") == "Fallback");
    CHECK(t.get("missing") == "missing");
    CHECK(t.get("title") == "¡Hola, señor!");            // UTF-8 as is
    CHECK(t.translate("{@hud.score}: 10 ({@menu.quit})") == "Puntos: 10 (Quit)");
    CHECK(t.translate("no keys {here}") == "no keys {here}");

    // In a game: the project's tables, ctx.text / ctx.format, and changing language while playing
    auto root = test::tempPath("strings_project");
    std::filesystem::create_directories(root / "strings");
    std::ofstream(root / "strings" / "en.json") << R"({"hello": "Hello", "count": "Coins"})";
    std::ofstream(root / "strings" / "es.json") << R"({"hello": "Hola"})";
    std::filesystem::path before = project::root();
    project::setRoot(root);
    CHECK(StringTable::languages(root) == std::vector<std::string>({"en", "es"}));
    test::ImGuiContextGuard imgui;
    Scene s;
    s.create("Thing");
    Game g;
    g.setLanguage("es");
    g.start(s, meshes());
    GameContext& ctx = g.context();
    CHECK(ctx.language() == "es");
    CHECK(ctx.text("hello") == "Hola");
    ctx.state().setNumber("coins", 3);
    CHECK(ctx.format("{@hello}! {@count}: {coins}") == "Hola! Coins: 3");
    ctx.setLanguage("en");
    CHECK(ctx.text("hello") == "Hello");
    ctx.setLanguage("fr");   // no table of its own: English, then the fallbacks
    CHECK(ctx.language() == "fr");
    CHECK(ctx.text("hello", "Bonjour") == "Hello");
    CHECK(ctx.text("only.here", "Bonjour") == "Bonjour");
    project::setRoot(before);
}

// Split screen: player cameras share the screen (by player number), saved with the scene; one alone doesn't split
TEST(game_split_screen_cameras) {
    Scene s;
    auto camera = [&](const char* name, int player, float x) {
        Entity& e = s.create(name);
        e.camera = Camera{};
        e.camera->player = player;
        e.transform.position[0] = x;
    };
    camera("Overview", 0, 0.f);
    camera("Two", 2, 20.f);
    camera("One", 1, 10.f);
    Game g;
    g.start(s, meshes());
    std::vector<CameraView> two = g.cameras();
    REQUIRE(two.size() == 2);
    CHECK(two[0].player == 1 && two[0].entity == s.findByName("One")->id);
    CHECK(two[0].rect[0] == 0.f && two[0].rect[2] == 0.5f && two[0].rect[3] == 1.f);
    CHECK(two[1].player == 2 && two[1].rect[0] == 0.5f);
    CHECK_NEAR(two[1].position.x, 20.0, 1e-4);
    g.stop();

    camera("Three", 3, 30.f);
    g.start(s, meshes());
    std::vector<CameraView> three = g.cameras();
    REQUIRE(three.size() == 3);
    CHECK(three[2].player == 3 && three[2].rect[0] == 0.f && three[2].rect[1] == 0.5f && three[2].rect[3] == 0.5f);
    CHECK(three[1].rect[0] == 0.5f && three[1].rect[1] == 0.f);
    g.stop();

    // Saved and loaded with the scene
    std::string error, path = test::tempPath("split.vkw").string();
    MeshLibrary lib = buildMeshLibrary();
    REQUIRE(saveScene(path, s, lib, textures(), error));
    Scene back;
    REQUIRE(loadScene(path, back, lib, textures(), error));
    CHECK(back.findByName("Three")->camera->player == 3);
    CHECK(back.findByName("Overview")->camera->player == 0);

    // One player camera alone: the usual single camera, all of the screen
    Scene single;
    Entity& only = single.create("Only");
    only.camera = Camera{};
    only.camera->player = 1;
    Game g2;
    g2.start(single, meshes());
    std::vector<CameraView> one = g2.cameras();
    REQUIRE(one.size() == 1);
    CHECK(one[0].rect[2] == 1.f && one[0].rect[3] == 1.f);
    g2.stop();
}

// The editor's string tables: every language loaded flat, edited, saved back nested by the dots (a key that is
// another's prefix stays flat), missing texts listed, English first
TEST(string_tables_edit_and_save) {
    std::filesystem::path root = test::tempPath("strings-project");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "strings");
    std::ofstream(root / "strings" / "en.json") << R"({"menu": {"resume": "Resume", "quit": "Quit"}, "hud.score": "Score"})";
    std::ofstream(root / "strings" / "ar.json") << R"({"menu": {"resume": "\u0627\u0633\u062a\u0626\u0646\u0627\u0641"}})";
    StringTables t;
    REQUIRE(t.load(root));
    REQUIRE(t.languages.size() == 2);
    CHECK(t.languages[0] == "en" && t.languages[1] == "ar");
    CHECK(t.text["menu.quit"]["en"] == "Quit");
    CHECK(t.missing("ar") == std::vector<std::string>({"hud.score", "menu.quit"}));
    CHECK(StringTables::rightToLeft(t.text["menu.resume"]["ar"]));
    CHECK(!StringTables::rightToLeft("Resume"));
    t.addLanguage("es");
    t.addLanguage("de");
    CHECK(t.languages == std::vector<std::string>({"en", "ar", "de", "es"}));
    t.text["menu.quit"]["es"] = "Salir";
    t.text["menu"]["en"] = "Menu";   // (a key that is also the others' prefix)
    std::string error;
    REQUIRE(t.save(root, error));
    // Read back by the game's own loader: nested and flat both come back the same
    StringTable es, en;
    REQUIRE(es.load(root, "es"));
    CHECK(es.get("menu.quit") == "Salir");
    CHECK(es.get("menu.resume") == "Resume");   // (from English)
    REQUIRE(en.load(root, "en"));
    CHECK(en.get("menu") == "Menu" && en.get("hud.score") == "Score" && en.get("menu.resume") == "Resume");
    json::Value doc;
    std::string text;
    std::ifstream f(root / "strings" / "es.json");
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    REQUIRE(json::parse(text, doc, error));
    CHECK(doc["menu"]["quit"].asString() == "Salir");   // (nested again)
    StringTables again;
    REQUIRE(again.load(root));
    CHECK(again.text["menu.quit"]["es"] == "Salir" && again.text["menu"]["en"] == "Menu");
    CHECK(!StringTables().load(test::tempPath("no-strings-here")));
}
