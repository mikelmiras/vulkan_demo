// AI (game/behaviour_tree.h): behaviour tree semantics, perception, walking, a guard, saving
#include "test.h"

#include "game/behaviour_tree.h"
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

int add(AIBrain& b, const char* type, std::string key = "", std::string text = "", float number = 0.f, float number2 = 0.f) {
    AIBrain::Node n;
    n.type = type;
    n.key = std::move(key);
    n.text = std::move(text);
    n.number = number;
    n.number2 = number2;
    b.nodes.push_back(n);
    return (int)b.nodes.size() - 1;
}

Vec3 pos(const Scene& s, const char* name) {
    Mat4 m = s.worldMatrix(s.findByName(name)->id);
    return {m.m[12], m.m[13], m.m[14]};
}

Entity& place(Scene& s, const char* name, Vec3 p) {
    Entity& e = s.create(name);
    e.transform.position[0] = p.x;
    e.transform.position[1] = p.y;
    e.transform.position[2] = p.z;
    return e;
}

}  // namespace

// Composites and decorators: counting with Set / Compare on the blackboard
TEST(ai_tree_semantics) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& e = s.create("Brain");
    AIBrain b;
    b.perceives.clear();
    int root = add(b, "Sequence");
    // Repeat 3 times: count += 1 (Compare + Set chains for 0, 1, 2), then wait 0.5 s, then set done
    int rep = add(b, "Repeat", "", "", 3.f);
    int inc = add(b, "Selector");
    int a0 = add(b, "Sequence"), c0 = add(b, "Compare", "count", "==", 0.f), s0 = add(b, "Set", "count", "1");
    int a1 = add(b, "Sequence"), c1 = add(b, "Compare", "count", "==", 1.f), s1 = add(b, "Set", "count", "2");
    int a2 = add(b, "Sequence"), c2 = add(b, "Compare", "count", "==", 2.f), s2 = add(b, "Set", "count", "3");
    int wait = add(b, "Wait", "", "", 0.5f);
    int done = add(b, "Set", "done", "true");
    int never = add(b, "Inverter");
    int yes = add(b, "Succeeder");
    int fail = add(b, "Has", "missing");
    b.nodes[(size_t)root].children = {rep, wait, done, never};
    b.nodes[(size_t)rep].children = {inc};
    b.nodes[(size_t)inc].children = {a0, a1, a2};
    b.nodes[(size_t)a0].children = {c0, s0};
    b.nodes[(size_t)a1].children = {c1, s1};
    b.nodes[(size_t)a2].children = {c2, s2};
    b.nodes[(size_t)never].children = {yes};
    b.nodes[(size_t)yes].children = {fail};
    b.blackboard.set("count", json::Value(0.0));
    e.brain = b;
    Game g;
    g.start(s, meshes());
    run(g, 1);
    const AIBrain& live = *s.findByName("Brain")->brain;
    CHECK(live.blackboard["count"].asNumber() == 1.0);   // (one repeat per update)
    run(g, 2);
    CHECK(live.blackboard["count"].asNumber() == 3.0);
    CHECK(!live.blackboard.has("done"));
    CHECK(live.status[(size_t)wait] == (int8_t)ai::Status::Running);
    run(g, 40);   // past the wait: done set, then the inverter of a success fails the sequence
    CHECK(live.blackboard["done"].asBool());
    CHECK(live.status[(size_t)root] == (int8_t)ai::Status::Failure);
    CHECK(ai::nodeType("MoveTo") && ai::nodeType("Selector")->children == -1 && !ai::nodeType("Nope"));
    // Values typed as text
    CHECK(ai::parseValue("12.5").asNumber() == 12.5);
    CHECK(ai::parseValue("true").asBool());
    CHECK(ai::parseValue("[1, 2, 3]").size() == 3);
    CHECK(ai::parseValue("Guard").asString() == "Guard");
    g.stop();
}

// Perception: seen inside the cone and range with nothing in the way; heard within range
TEST(ai_perception) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& guard = place(s, "Guard", {0, 0, 0});   // looking down -Z
    guard.brain = AIBrain{};
    guard.brain->sightRange = 10.f;
    guard.brain->sightAngle = 90.f;
    guard.brain->hearingRange = 8.f;
    Entity& player = place(s, "Player", {0, 0, -6});
    player.tags.push_back("Player");
    Game g;
    g.start(s, meshes());
    run(g, 1);
    // (looked up each time: adding entities moves them)
    auto brain = [&]() -> const AIBrain& { return *s.findByName("Guard")->brain; };
    CHECK(brain().blackboard["seeing"].asBool());
    CHECK(brain().blackboard["target"].asString() == "Player");
    CHECK_NEAR(brain().blackboard["lastSeen"][2].asFloat(), -6.0, 1e-4);
    auto movePlayer = [&](Vec3 p) {
        Entity* e = s.findByName("Player");
        e->transform.position[0] = p.x;
        e->transform.position[1] = p.y;
        e->transform.position[2] = p.z;
        run(g, 1);
    };
    movePlayer({0, 0, 6});   // behind
    CHECK(!brain().blackboard["seeing"].asBool());
    CHECK(brain().blackboard["target"].asString() == "Player");   // (remembered for a while)
    movePlayer({8, 0, -3});   // to the side, outside the 90-degree cone
    CHECK(!brain().blackboard["seeing"].asBool());
    movePlayer({0, 0, -12});   // in front, too far
    CHECK(!brain().blackboard["seeing"].asBool());
    // A wall in between
    Entity& wall = place(s, "Wall", {0, 1, -3});
    wall.collider = Collider{};
    wall.collider->size = {4, 4, 0.5f};
    movePlayer({0, 0, -6});   // (its body is made by the physics step after this update's perception)
    run(g, 1);
    CHECK(!brain().blackboard["seeing"].asBool());
    s.remove(s.findByName("Wall")->id);
    movePlayer({0, 0, -6});   // (its body goes with the physics step after this update's perception)
    run(g, 1);
    CHECK(brain().blackboard["seeing"].asBool());
    // Hearing: a sound near enough, and one too far
    g.context().playSound("Beep", {20, 0, 0});
    run(g, 1);
    CHECK(!brain().blackboard.has("noise"));
    g.context().makeNoise({5, 0, 2}, 1.f);
    run(g, 1);
    REQUIRE(brain().blackboard.has("noise"));
    CHECK_NEAR(brain().blackboard["noise"][0].asFloat(), 5.0, 1e-4);
    // Forgotten after its memory runs out
    movePlayer({0, 0, 6});
    run(g, (int)(60 * 6));
    CHECK(!brain().blackboard.has("target"));
    g.stop();
}

// A guard: patrols between two posts, chases the player when it sees them, gives up when they're gone
TEST(ai_guard_patrols_and_chases) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& ground = place(s, "Ground", {0, -0.5f, 0});
    ground.collider = Collider{};
    ground.collider->size = {60, 1, 60};
    place(s, "Post A", {-8, 0, 0});
    place(s, "Post B", {8, 0, 0});
    Entity& guard = place(s, "Guard", {-8, 0, 0});
    guard.brain = AIBrain{};
    ai::makeGuard(*guard.brain, "Post A, Post B");
    guard.brain->sightRange = 12.f;
    guard.brain->sightAngle = 120.f;
    Entity& player = place(s, "Player", {0, 0, 25});   // far away, unseen
    player.tags.push_back("Player");
    Game g;
    g.start(s, meshes());
    // Patrol: it gets to post B, then back towards A
    float closestB = 1e9f;
    for (int i = 0; i < 60 * 12; ++i) {
        run(g, 1);
        closestB = std::min(closestB, length(pos(s, "Guard") - pos(s, "Post B")));
    }
    CHECK(closestB < 1.f);
    // The player appears in front of it: it comes after them
    Vec3 at = pos(s, "Guard");
    Vec3 fwd = normalize(transformDir(s.worldMatrix(s.findByName("Guard")->id), {0, 0, -1}));
    Entity* p = s.findByName("Player");
    Vec3 there = at + fwd * 7.f;
    p->transform.position[0] = there.x;
    p->transform.position[2] = there.z;
    run(g, 60 * 3);
    CHECK(s.findByName("Guard")->brain->blackboard["seeing"].asBool());
    CHECK(length(pos(s, "Guard") - pos(s, "Player")) < 2.f);   // (stops 1.5 m short)
    g.stop();
}

TEST(ai_brain_saved) {
    Scene s;
    Entity& e = s.create("Guard");
    e.brain = AIBrain{};
    ai::makeGuard(*e.brain, "A, B");
    e.brain->blackboard.set("alert", json::Value(2.0));
    e.brain->sightAngle = 75.f;
    MeshLibrary lib = buildMeshLibrary();
    TextureLibrary tex;
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(sceneToJson(s, lib, tex), back, lib, tex, error));
    const Entity* r = back.findByName("Guard");
    REQUIRE(r && r->brain);
    CHECK(r->brain->nodes.size() == e.brain->nodes.size());
    CHECK(r->brain->nodes[0].children == e.brain->nodes[0].children);
    CHECK(r->brain->blackboard["alert"].asNumber() == 2.0);
    CHECK(r->brain->sightAngle == 75.f);
    CHECK(sameEntity(*r, *s.findByName("Guard")));
}
