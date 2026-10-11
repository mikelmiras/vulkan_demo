// Scene flow: changing scenes from game code, state that survives it, quitting, save games
#include "test.h"

#include "core/project.h"
#include "game/game.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

void run(Game& g, int frames) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    }
}

bool logged(const Game& g, const std::string& text) {
    for (const auto& line : g.log())
        if (line.second.find(text) != std::string::npos) return true;
    return false;
}

// Counts its visits in the game state and moves on to `next` after `after` updates
struct Traveller : BehaviourBase<Traveller> {
    static constexpr const char* kTypeName = "TestTraveller";
    std::string next;
    float after = 3.f;
    float ticks = 0.f;   // runtime state, saved in save games
    void fields(FieldVisitor& v) override {
        v.field("next", next);
        v.field("after", after);
    }
    void state(FieldVisitor& v) override { v.field("ticks", ticks); }
    void start(GameContext& ctx, Entity&) override { ctx.state().add("visits", 1.f); }
    void update(GameContext& ctx, Entity&) override {
        if (++ticks == after && !next.empty()) {
            if (next == "quit") ctx.quit();
            else ctx.loadScene(next);
        }
    }
};

void registerTraveller() {
    static bool done = false;
    if (!done) registerBehaviour<Traveller>("test");
    done = true;
}

// A scene file with one entity carrying a Traveller to `next`
std::string writeLevel(const std::string& name, const std::string& next, MeshLibrary& meshes) {
    registerTraveller();
    Scene s;
    Entity& e = s.create(name);
    auto t = std::make_unique<Traveller>();
    t->next = next;
    e.behaviours.emplace_back(std::move(t));
    std::string path = test::tempPath(name + ".vkw").string(), error;
    if (!saveScene(path, s, meshes, textures(), error)) std::printf("    %s\n", error.c_str());
    return path;
}

}  // namespace

TEST(scene_flow_change_scene_keeps_state) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    std::string b = writeLevel("LevelB", "", meshes);
    std::string a = writeLevel("LevelA", b, meshes);
    Scene s;
    std::string error;
    REQUIRE(loadScene(a, s, meshes, textures(), error));
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.setScenePath(a);
    g.start(s, meshes);
    CHECK_NEAR(g.state().number("visits"), 1.0, 0.0);
    run(g, 5);
    CHECK(s.findByName("LevelB") != nullptr);   // the scene object now holds level B
    CHECK(s.findByName("LevelA") == nullptr);
    CHECK(g.scenePath() == b);
    CHECK_NEAR(g.state().number("visits"), 2.0, 0.0);   // state carried over
    CHECK(g.time() > 0.07f);                            // the clock too
}

TEST(scene_flow_bad_scene_is_logged_and_game_goes_on) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    std::string a = writeLevel("LevelBad", test::tempPath("no_such_level.vkw").string(), meshes);
    Scene s;
    std::string error;
    REQUIRE(loadScene(a, s, meshes, textures(), error));
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.start(s, meshes);
    run(g, 5);
    CHECK(logged(g, "loadScene"));
    CHECK(s.findByName("LevelBad") != nullptr);
    CHECK(g.running());
}

TEST(scene_flow_quit) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    std::string error;
    REQUIRE(loadScene(writeLevel("LevelQuit", "quit", meshes), s, meshes, textures(), error));
    Game g;
    g.start(s, meshes);
    CHECK(!g.quitRequested());
    run(g, 5);
    CHECK(g.quitRequested());
}

// A save game restores where things were, the behaviours' runtime state, the game state and the clock
TEST(scene_flow_save_and_load_game) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    registerTraveller();
    Scene s = buildDefaultScene();
    Entity& t = s.create("Traveller");
    t.behaviours.emplace_back(std::make_unique<Traveller>());
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.setSaveDirectory(test::tempPath("saves"));
    g.start(s, meshes);
    g.state().setText("weapon", "laser");
    run(g, 40);
    auto pos = [&](const char* name) {
        Mat4 m = s.worldMatrix(s.findByName(name)->id);
        return Vec3{m.m[12], m.m[13], m.m[14]};
    };
    Vec3 sphere = pos("Centre Sphere");   // bobbing: depends on the time and its start height
    float time = g.time();
    std::string error;
    REQUIRE(g.saveGame("slot1", error));
    REQUIRE(std::filesystem::exists(g.saveGamePath("slot1")));
    size_t count = s.entities.size();

    run(g, 50);
    g.state().setText("weapon", "spoon");
    s.findByName("Player")->transform.position[0] = 25.f;
    REQUIRE(g.loadGame("slot1", error));
    CHECK(s.entities.size() == count);
    CHECK_NEAR(g.time(), time, 1e-4);
    CHECK(g.state().text("weapon") == "laser");
    CHECK_VEC(pos("Centre Sphere"), sphere, 1e-3f);
    CHECK_NEAR(s.findByName("Player")->transform.position[0], 0.f, 1e-3);
    Traveller* tr = s.findByName("Traveller")->behaviour<Traveller>();
    REQUIRE(tr);
    CHECK_NEAR(tr->ticks, 40.0, 0.5);   // its runtime state, not a fresh start()
    // Bob keeps bobbing around the height it started at, not where the save caught it
    run(g, 1);
    CHECK(std::fabs(pos("Centre Sphere").y - sphere.y) < 0.05f);
    CHECK(!g.loadGame("nope", error));
}

// The built-in SceneExit and SaveLoadKeys behaviours, driven like a player would
TEST(scene_flow_builtin_exit_and_quicksave) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    std::string target = writeLevel("LevelExitTarget", "", meshes);
    Scene s = buildDefaultScene();
    Entity& exit = s.create("Exit");
    exit.transform.position[2] = 34.f;   // where the player stands
    Collider c;
    c.shape = Collider::Shape::Sphere;
    c.radius = 3.f;
    c.trigger = true;
    exit.collider = c;
    auto b = createBehaviour("SceneExit");
    REQUIRE(b);
    CHECK(setField(*b, "scene", target));
    exit.behaviours.emplace_back(std::move(b));
    Entity& keys = s.create("Keys");
    keys.behaviours.emplace_back(createBehaviour("SaveLoadKeys"));

    Game g;
    g.setAssetLibraries(meshes, textures());
    g.setSaveDirectory(test::tempPath("saves2"));
    g.start(s, meshes);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_F5, true);   // quicksave on the first update
    run(g, 1);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_F5, false);
    run(g, 3);
    CHECK(logged(g, "Saved 'quicksave'"));
    CHECK(s.findByName("LevelExitTarget") != nullptr);   // the player was in the exit's trigger
}

// Scenes saved before the quicksave actions existed get them; newer ones keep exactly their own list
TEST(scene_flow_old_scenes_get_new_actions) {
    MeshLibrary meshes = buildMeshLibrary();
    auto path = test::tempPath("old_actions.vkw");
    test::writeFile(path, R"({"format":"vkworld-scene","version":8,"entities":[],
        "settings":{"input":[{"name":"jump","keys":["Space"]}]}})");
    Scene s;
    std::string error;
    REQUIRE(loadScene(path.string(), s, meshes, textures(), error));
    CHECK(s.inputActions.size() == 3);
    CHECK(s.inputActions[1].name == "quicksave");
}
