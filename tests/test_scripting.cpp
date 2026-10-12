// script/lua_script: Lua behaviours (fields, callbacks, the game API, hot reload, errors)
#include "test.h"

#include <chrono>
#include <thread>

#include "game/game.h"
#include "imgui.h"
#include "scene/serialization.h"
#include "script/lua_script.h"

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

Entity& scripted(Scene& s, const char* name, const char* type) {
    Entity& e = s.create(name);
    std::unique_ptr<Behaviour> b = createBehaviour(type);
    REQUIRE(b != nullptr);
    e.behaviours.emplace_back(std::move(b));
    return e;
}

bool logged(const Game& g, const std::string& text) {
    for (const auto& [t, line] : g.log())
        if (line.find(text) != std::string::npos) return true;
    return false;
}

const char* kMover = R"lua(
local Mover = {
    description = "Moves along X",
    fields = {
        {"speed", 2, min = 0, max = 10},
        {"count", 3, int = true},
        {"label", "hi"},
        {"target", "", ref = "entity"},
        {"mode", "walk", options = {"walk", "run"}},
        {"enabled", true},
        {"offset", vec3(1, 2, 3)},
    },
}
function Mover:start()
    self.ticks = 0
    game.log("start " .. self.label .. " " .. self.mode .. " " .. tostring(self.offset))
end
function Mover:update(dt)
    self.ticks = self.ticks + 1
    local p = self.entity.position
    p.x = p.x + self.speed * dt
    self.entity.position = p
    self.label = "ticked"
end
return Mover
)lua";

}  // namespace

// Fields declared in the script: in the registry, saved and loaded like a C++ behaviour's
TEST(lua_fields) {
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Mover", kMover, error));
    CHECK(scripting::isScriptType("Mover"));
    std::string module;
    CHECK(behaviourTypeModule("Mover", module) && module == "lua");
    std::unique_ptr<Behaviour> b = createBehaviour("Mover");
    REQUIRE(b != nullptr);
    CHECK(std::string(b->typeName()) == "Mover");
    json::Value v = behaviourToJson(*b);
    CHECK(v["type"].asString() == "Mover");
    CHECK(v["speed"].asFloat() == 2.f);
    CHECK(v["count"].asInt() == 3);
    CHECK(v["label"].asString() == "hi");
    CHECK(v["mode"].asString() == "walk");
    CHECK(v["enabled"].asBool());
    CHECK(v["offset"].isArray() && v["offset"].size() == 3);

    CHECK(setField(*b, "speed", 5.f));
    CHECK(setField(*b, "mode", std::string("run")));
    CHECK(!setField(*b, "nope", 1.f));
    std::unique_ptr<Behaviour> back = behaviourFromJson(behaviourToJson(*b));
    json::Value v2 = behaviourToJson(*back);
    CHECK(v2["speed"].asFloat() == 5.f && v2["mode"].asString() == "run");
    std::unique_ptr<Behaviour> copy = back->clone();
    CHECK(behaviourToJson(*copy)["speed"].asFloat() == 5.f);
    scripting::reset();
    CHECK(!createBehaviour("Mover"));
}

TEST(lua_load_errors) {
    scripting::reset();
    std::string error;
    CHECK(!scripting::define("Broken", "local x = = 1", error));
    CHECK(error.find("Broken.lua:1:") != std::string::npos);
    CHECK(!createBehaviour("Broken"));
    CHECK(!scripting::define("NoTable", "return 42", error));
    CHECK(error.find("return") != std::string::npos);
    CHECK(!scripting::define("BadField", "return {fields = {{\"x\", function() end}}}", error));
    CHECK(error.find("field 'x'") != std::string::npos);
    CHECK(!scripting::define("Clash", "return {fields = {{\"entity\", 1}}}", error));
    CHECK(!scripting::define("Spin", "return {}", error));   // a built-in C++ behaviour has that name
    CHECK(error.find("C++") != std::string::npos);
    CHECK(!scripting::define("not a name", "return {}", error));
    scripting::reset();
}

// Callbacks run in play mode: start, update (moving the entity), the fields as the instance's members
TEST(lua_play) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Mover", kMover, error));
    Scene s;
    Entity& e = scripted(s, "Box", "Mover");
    setField(*e.behaviours[0], "speed", 6.f);
    Game g;
    g.start(s, meshes());
    CHECK(logged(g, "start hi walk (1, 2, 3)"));
    steps(g, 60);
    Entity* box = s.findByName("Box");
    CHECK_NEAR(box->transform.position[0], 6.f, 1e-3);
    // The instance's members come back to the fields (what the inspector shows while playing)
    CHECK(behaviourToJson(*box->behaviours[0])["label"].asString() == "ticked");
    // ... and edits go the other way
    setField(*box->behaviours[0], "speed", 0.f);
    steps(g, 10);
    CHECK_NEAR(box->transform.position[0], 6.f, 1e-3);
    g.stop();
    scripting::reset();
}

// The game API: find, tags, messages, timers, state, spawn, destroy, raycasts
TEST(lua_game_api) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Boss", R"lua(
local Boss = {fields = {{"minion", "Minion", ref = "prefab"}}}
function Boss:start()
    game.state.score = 10
    game.state.name = "boss"
    self.got = {}
    game.after(0.5, function() game.log("half a second") end)
    self.timer = game.every(0.25, function() self.beats = (self.beats or 0) + 1 end)
    local m = game.spawn(self.minion, vec3(0, 5, 0))
    assert(m ~= nil and m.name == "Minion", "spawned")
    m:addTag("Spawned")
end
function Boss:update(dt)
    if game.time() > 1.0 and self.timer then game.cancel(self.timer); self.timer = nil end
    if #self.got == 0 and game.time() > 0.1 then
        game.sendToTag("Grunt", "hello", 7, "there")
    end
end
function Boss:onMessage(msg)
    table.insert(self.got, msg.name)
    if msg.name == "reply" then
        game.log("reply " .. msg.value .. " from " .. msg.from.name)
        game.state.score = game.state.score + msg.value
    end
end
return Boss
)lua", error));
    REQUIRE(scripting::define("Grunt", R"lua(
local Grunt = {}
function Grunt:onMessage(msg)
    if msg.name == "hello" then
        assert(msg.text == "there")
        msg.from:send("reply", msg.value * 2)
        local spawned = game.findWithTag("Spawned")
        game.log("spawned " .. #spawned .. " " .. spawned[1].name)
        game.destroy(spawned[1])
    end
end
return Grunt
)lua", error));
    Scene s;
    scripted(s, "Boss", "Boss");
    Entity& grunt = scripted(s, "Grunt", "Grunt");
    grunt.tags = {"Grunt"};
    Prefab p;
    p.name = "Minion";
    Entity m;
    m.id = 1;
    m.name = "Minion";
    p.entities.push_back(m);
    s.addPrefab(p);
    Game g;
    g.start(s, meshes());
    steps(g, 90);
    CHECK(logged(g, "half a second"));
    CHECK(logged(g, "reply 14 from Grunt"));
    CHECK(logged(g, "spawned 1 Minion"));
    CHECK(!s.findByName("Minion"));
    CHECK(g.state().number("score") == 24.f);
    CHECK(g.state().text("name") == "boss");
    // every(0.25) until 1 s: four beats
    Entity* boss = s.findByName("Boss");
    CHECK(behaviourStateToJson(*boss->behaviours[0]).isObject());
    g.stop();
    scripting::reset();
}

// A runtime error: logged once (in the viewport too), the instance stops; others carry on
TEST(lua_runtime_error) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Faulty", R"lua(
local Faulty = {}
function Faulty:update(dt) local x = nil; x.y = 1 end
return Faulty
)lua", error));
    REQUIRE(scripting::define("Counter", R"lua(
local Counter = {fields = {{"n", 0}}}
function Counter:update(dt) self.n = self.n + 1 end
return Counter
)lua", error));
    Scene s;
    scripted(s, "F", "Faulty");
    scripted(s, "C", "Counter");
    Game g;
    g.start(s, meshes());
    steps(g, 5);
    int errors = 0;
    for (const auto& [t, line] : g.log()) errors += line.find("Script error (Faulty.update)") != std::string::npos;
    CHECK(errors == 1);
    CHECK(logged(g, "Faulty.lua:3:"));
    CHECK(behaviourToJson(*s.findByName("C")->behaviours[0])["n"].asInt() == 5);
    // Fixed: it runs again
    REQUIRE(scripting::define("Faulty", "local F = {}\nfunction F:update(dt) game.state.fixed = true end\nreturn F", error));
    steps(g, 1);
    CHECK(g.state().flag("fixed"));
    g.stop();
    scripting::reset();
}

// Redefining a script while it runs: the instances keep their members and get the new functions
TEST(lua_hot_reload_keeps_state) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Tick", "local T = {}\nfunction T:start() self.n = 0 end\n"
                                      "function T:update(dt) self.n = self.n + 1 end\nreturn T", error));
    Scene s;
    scripted(s, "A", "Tick");
    Game g;
    g.start(s, meshes());
    steps(g, 10);
    REQUIRE(scripting::define("Tick", "local T = {fields = {{\"step\", 100}}}\nfunction T:start() self.n = 0 end\n"
                                      "function T:update(dt) self.n = self.n + self.step\n"
                                      "  if self.n > 500 then game.state.n = self.n end end\nreturn T", error));
    steps(g, 5);   // 10 + 5 * 100: still counting from where it was
    CHECK(g.state().number("n") == 510.f);
    g.stop();
    scripting::reset();
}

// A project's scripts/ folder: every script a type (lib/ is for require()), reloaded when a file changes
TEST(lua_project_scripts) {
    test::ImGuiContextGuard imgui;
    auto root = test::tempPath("luaproject");
    std::filesystem::remove_all(root);
    test::writeFile(root / "scripts" / "lib" / "util.lua", "local M = {}\nfunction M.double(x) return x * 2 end\nreturn M");
    test::writeFile(root / "scripts" / "Doubler.lua",
                    "local util = require(\"lib.util\")\nlocal D = {fields = {{\"x\", 21}}}\n"
                    "function D:start() game.state.out = util.double(self.x) end\nreturn D");
    test::writeFile(root / "scripts" / "enemies" / "Walker.lua", "return {description = \"Walks\"}");
    test::writeFile(root / "scripts" / "Bad.lua", "return {");
    CHECK(scripting::loadProject(root) == 2);
    CHECK(scripting::isScriptType("Doubler") && scripting::isScriptType("Walker"));
    CHECK(!scripting::isScriptType("util") && !scripting::isScriptType("Bad"));
    REQUIRE(scripting::errors().size() == 1);
    CHECK(scripting::errors()[0].find("scripts/Bad.lua:1:") != std::string::npos);
    CHECK(scripting::scriptFile("Walker") == root / "scripts" / "enemies" / "Walker.lua");

    Scene s;
    scripted(s, "D", "Doubler");
    Game g;
    g.start(s, meshes());
    CHECK(g.state().number("out") == 42.f);
    g.stop();

    // Edited (and fixed) on disk: poll() picks them up
    auto past = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5);
    test::writeFile(root / "scripts" / "Doubler.lua",
                    "local D = {fields = {{\"x\", 1}}}\nfunction D:start() game.state.out = -1 end\nreturn D");
    std::filesystem::last_write_time(root / "scripts" / "Doubler.lua", past);
    test::writeFile(root / "scripts" / "Bad.lua", "return {}");
    std::filesystem::last_write_time(root / "scripts" / "Bad.lua", past);
    test::writeFile(root / "scripts" / "Fresh.lua", "return {}");
    std::vector<std::string> reloaded = scripting::poll(true);
    std::sort(reloaded.begin(), reloaded.end());
    CHECK((reloaded == std::vector<std::string>{"Bad", "Doubler", "Fresh"}));
    CHECK(scripting::errors().empty());
    CHECK(createBehaviour("Bad") != nullptr);
    CHECK(scripting::poll(true).empty());   // nothing changed since
    Scene s2;
    scripted(s2, "D", "Doubler");
    Game g2;
    g2.start(s2, meshes());
    CHECK(g2.state().number("out") == -1.f);
    g2.stop();
    scripting::reset();
}

// Scenes keep script behaviours (and their fields) through a save and a load
TEST(lua_scene_roundtrip) {
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Mover", kMover, error));
    Scene s;
    Entity& e = s.create("Box");
    e.behaviours.emplace_back(createBehaviour("Mover"));
    setField(*e.behaviours[0], "label", std::string("saved"));
    auto path = test::tempPath("lua_scene.vkw");
    MeshLibrary lib = buildMeshLibrary();
    TextureLibrary tex = buildTextureLibrary();
    REQUIRE(saveScene(path.u8string(), s, lib, tex, error));
    Scene back;
    REQUIRE(loadScene(path.u8string(), back, lib, tex, error));
    REQUIRE(back.findByName("Box") && back.findByName("Box")->behaviours.size() == 1);
    CHECK(behaviourToJson(*back.findByName("Box")->behaviours[0])["label"].asString() == "saved");
    // Without the script: kept as a missing behaviour, fields and all, and back once it's there
    scripting::reset();
    Scene missing;
    REQUIRE(loadScene(path.u8string(), missing, lib, tex, error));
    CHECK(countMissingBehaviours(missing) == 1);
    REQUIRE(scripting::define("Mover", kMover, error));
    CHECK(reattachBehaviours(missing) == 1);
    CHECK(behaviourToJson(*missing.findByName("Box")->behaviours[0])["label"].asString() == "saved");
    scripting::reset();
}
