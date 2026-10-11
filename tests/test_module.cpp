// game/module: loading a project's code, playing with it, and swapping it while scenes hold its behaviours
#include "test.h"

#include "core/json.h"
#include "core/project.h"
#include "game/game.h"
#include "game/module.h"
#include "imgui.h"
#include "scene/serialization.h"

#ifdef VKWORLD_SAMPLE_GAME

namespace fs = std::filesystem;

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

fs::path sampleModule() { return GameModule::libraryPath(fs::u8path(VKWORLD_SAMPLE_GAME) / "bin" / "sample_game"); }

bool loadSampleScene(Scene& s) {
    MeshLibrary lib = buildMeshLibrary();
    std::string error;
    project::setRoot(fs::u8path(VKWORLD_SAMPLE_GAME));
    bool ok = loadScene((fs::u8path(VKWORLD_SAMPLE_GAME) / "scenes" / "main.vkw").u8string(), s, lib, textures(), error);
    project::setRoot(fs::current_path());
    if (!ok) std::printf("    %s\n", error.c_str());
    return ok;
}

bool logged(const Game& g, const std::string& text) {
    for (const auto& line : g.log())
        if (line.second.find(text) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST(module_loads_and_registers_behaviours) {
    GameModule module;
    std::string error;
    REQUIRE(module.load(sampleModule(), error));
    CHECK(module.behaviourTypes().size() == 4);
    CHECK(createBehaviour("Coin") != nullptr);
    CHECK(createBehaviour("Spin") != nullptr);   // the built-ins are still there
    module.unload();
    CHECK(createBehaviour("Coin") == nullptr);
    CHECK(createBehaviour("Spin") != nullptr);
}

// The sample scene played with the module's code: walking into a coin scores
TEST(module_behaviours_play) {
    test::ImGuiContextGuard imgui;
    GameModule module;
    std::string error;
    REQUIRE(module.load(sampleModule(), error));
    {
        Scene s;
        REQUIRE(loadSampleScene(s));
        CHECK(countMissingBehaviours(s) == 0);
        Game g;
        g.start(s, meshes());
        CHECK_NEAR(g.state().number("target"), 8.0, 0.0);
        const Entity* coin = s.findByName("Coin.001");
        REQUIRE(coin);
        EntityId coinId = coin->id;
        Mat4 m = s.worldMatrix(coinId);
        Entity* player = s.findByName("Player");
        REQUIRE(player);
        player->transform.position[0] = m.m[12];
        player->transform.position[1] = 0.f;
        player->transform.position[2] = m.m[14];
        for (int i = 0; i < 5; ++i) {
            ImGui::NewFrame();
            g.step();
            ImGui::EndFrame();
        }
        CHECK_NEAR(g.state().number("score"), 1.0, 0.0);
        CHECK(!s.find(coinId));   // collected
        g.stop();
    }   // the scene (and its behaviours) go before the module
}

// Collecting enough coins finishes level 1; level 2 loads with the score carried over
TEST(module_sample_levels_and_score_carry_over) {
    test::ImGuiContextGuard imgui;
    GameModule module;
    std::string error;
    REQUIRE(module.load(sampleModule(), error));
    MeshLibrary meshes = buildMeshLibrary();
    {
        Scene s;
        REQUIRE(loadSampleScene(s));
        project::setRoot(fs::u8path(VKWORLD_SAMPLE_GAME));
        Game g;
        g.setAssetLibraries(meshes, textures());
        g.setScenePath("scenes/main.vkw");
        g.start(s, meshes);
        auto step = [&](int n) {
            for (int i = 0; i < n; ++i) {
                ImGui::NewFrame();
                g.step();
                ImGui::EndFrame();
            }
        };
        for (int c = 1; c <= 8; ++c) {   // teleport the player onto 8 coins
            char name[16];
            std::snprintf(name, sizeof(name), "Coin.%03d", c);
            const Entity* coin = s.findByName(name);
            REQUIRE(coin);
            Mat4 m = s.worldMatrix(coin->id);
            Entity* player = s.findByName("Player");
            player->transform.position[0] = m.m[12];
            player->transform.position[1] = 0.f;
            player->transform.position[2] = m.m[14];
            step(3);
        }
        CHECK(logged(g, "Level complete!"));
        step(150);   // 2 s later: the next level
        project::setRoot(fs::current_path());
        CHECK(g.scenePath() == "scenes/level2.vkw");
        CHECK_NEAR(g.state().number("level"), 2.0, 0.0);
        CHECK_NEAR(g.state().number("target"), 20.0, 0.0);
        CHECK_NEAR(g.state().number("score"), 8.0, 1.0);   // carried over (a drone may have taken one)
        CHECK(s.findByName("Drone.3") != nullptr);           // level 2 has three
        g.stop();
    }
}

// What the editor does when the module is rebuilt: behaviours keep their (edited) fields across the swap
TEST(module_reload_keeps_behaviours) {
    GameModule module;
    std::string error;
    REQUIRE(module.load(sampleModule(), error));
    Scene s;
    REQUIRE(loadSampleScene(s));
    Entity* coin = s.findByName("Coin.002");
    REQUIRE(coin && !coin->behaviours.empty());
    CHECK(setField(*coin->behaviours[0], "points", 7.f));

    CHECK(detachBehaviours(s, module.behaviourTypes()) == 19);   // 12 coins, 2 drones, 4 lurkers, the score keeper
    module.unload();
    REQUIRE(module.load(sampleModule(), error));
    CHECK(reattachBehaviours(s) == 19);
    CHECK(countMissingBehaviours(s) == 0);
    coin = s.findByName("Coin.002");
    json::Value saved = behaviourToJson(*coin->behaviours[0]);
    CHECK(saved["type"].asString() == "Coin");
    CHECK_NEAR(saved["points"].asFloat(0.f), 7.0, 1e-6);
    s = Scene{};
}

TEST(module_rejects_non_modules) {
    GameModule module;
    std::string error;
    CHECK(!module.load(test::tempPath("nothing.so"), error));
    CHECK(!error.empty());
    auto fake = test::tempPath("fake_module.so");
    test::writeFile(fake, "not a library");
    error.clear();
    CHECK(!module.load(fake, error));
    CHECK(!error.empty());
    CHECK(!module.loaded());
}

#endif
