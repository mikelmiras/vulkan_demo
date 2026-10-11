// game/navigation: baking the walkable grid, paths around walls, steps and stacked floors, the NavAgent
#include "test.h"

#include "game/game.h"
#include "game/navigation.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

Entity& box(Scene& s, const char* name, Vec3 pos, Vec3 size) {
    Entity& e = s.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    e.transform.scale[0] = size.x; e.transform.scale[1] = size.y; e.transform.scale[2] = size.z;
    e.collider = Collider{};
    return e;
}

// A 30 x 30 m floor (top at y = 0) split by a wall along x = 0 with a gap at z = 10..12; stairs up to a 1.2 m
// platform in one corner; a bridge 3 m up; a tall block nothing can climb
Scene level() {
    Scene s;
    box(s, "Floor", {0, -0.5f, 0}, {30, 1, 30});
    box(s, "Wall A", {0, 1.5f, -2.5f}, {0.5f, 3, 25});   // z -15 .. 10
    box(s, "Wall B", {0, 1.5f, 13.5f}, {0.5f, 3, 3});    // z 12 .. 15
    for (int i = 0; i < 4; ++i)                          // stairs: 0.3 m steps going +x, at z -12 .. -9
        box(s, "Step", {6.f + i * 0.6f + 0.3f, 0.15f + i * 0.15f, -10.5f}, {0.6f, 0.3f + i * 0.3f, 3});
    box(s, "Platform", {9.9f, 0.6f, -10.5f}, {3, 1.2f, 3});
    box(s, "Bridge", {-8, 3.f, 0}, {3, 0.2f, 12});       // top at 3.1, z -6 .. 6
    box(s, "Block", {-8, 1.25f, -12}, {3, 2.5f, 3});     // 2.5 m high: an island on top
    return s;
}

float pathLength(const std::vector<Vec3>& p) {
    float l = 0.f;
    for (size_t i = 1; i < p.size(); ++i) l += length(p[i] - p[i - 1]);
    return l;
}

}  // namespace

TEST(navigation_bake) {
    Scene s = level();
    NavGrid nav;
    REQUIRE(nav.bake(s, meshes(), s.navigation));
    CHECK(nav.cellCount() > 1000);
    Vec3 p;
    CHECK(nav.nearest({5, 1, 5}, p, 0.f));      // open floor
    CHECK_NEAR(p.y, 0.0, 1e-3);
    CHECK(!nav.nearest({0, 1, 5}, p, 0.f));     // in the wall
    CHECK(!nav.nearest({0.35f, 1, 5}, p, 0.f)); // too close to it (agent radius)
    CHECK(nav.nearest({-8, 3.5f, 0}, p, 0.f));  // on the bridge ...
    CHECK_NEAR(p.y, 3.1, 1e-3);
    CHECK(nav.nearest({-8, 1, 0}, p, 0.f));     // ... and under it
    CHECK_NEAR(p.y, 0.0, 1e-3);
    // Moving things and characters aren't the level
    Entity& crate = box(s, "Crate", {5, 0.5f, 5}, {1, 1, 1});
    crate.rigidBody = RigidBody{};
    NavGrid again;
    REQUIRE(again.bake(s, meshes(), s.navigation));
    CHECK(again.nearest({5, 1, 5}, p, 0.f));
    CHECK(again.cellCount() == nav.cellCount());
}

TEST(navigation_paths) {
    Scene s = level();
    NavGrid nav;
    REQUIRE(nav.bake(s, meshes(), s.navigation));
    std::vector<Vec3> path;
    // Across the wall: through the gap
    REQUIRE(nav.findPath({5, 0.9f, 0}, {-5, 0.9f, 0}, path));
    REQUIRE(path.size() >= 3);
    CHECK_VEC(path.front(), test::v3(5, 0, 0), 1e-3f);
    CHECK_VEC(path.back(), test::v3(-5, 0, 0), 1e-3f);
    CHECK(pathLength(path) > 20.f);   // straight would be 10
    CHECK(pathLength(path) < 30.f);   // and it's straightened, not a staircase of cells
    bool throughGap = false;
    for (size_t i = 1; i < path.size(); ++i) {
        CHECK(nav.walkable(path[i - 1], path[i]));
        if ((path[i - 1].x > 0.f) != (path[i].x > 0.f)) {   // crossing x = 0: where?
            float t = path[i - 1].x / (path[i - 1].x - path[i].x);
            float z = path[i - 1].z + (path[i].z - path[i - 1].z) * t;
            throughGap = z > 10.f && z < 12.f;
        }
    }
    CHECK(throughGap);
    // Open floor: a straight line
    REQUIRE(nav.findPath({2, 0, -2}, {12, 0, 6}, path));
    CHECK(path.size() == 2);
    // Up the stairs onto the platform
    REQUIRE(nav.findPath({3, 0, -10.5f}, {9.9f, 1.2f, -10.5f}, path));
    CHECK_NEAR(path.back().y, 1.2, 1e-3);
    // Onto the bridge: no way up there, nor onto the block
    CHECK(!nav.findPath({5, 0, 0}, {-8, 3.1f, 0}, path));
    CHECK(!nav.findPath({5, 0, 0}, {-8, 2.5f, -12}, path));
    // From somewhere not on the grid (inside the wall)
    CHECK(!nav.findPath({0, 1, -5}, {5, 0, 0}, path) || path.size() >= 2);
    CHECK(!nav.walkable({2, 0, 0}, {-2, 0, 0}));
}

TEST(navigation_settings_saved) {
    Scene s = level();
    s.navigation.cellSize = 0.25f;
    s.navigation.agentRadius = 0.6f;
    s.navigation.maxSlope = 30.f;
    std::string error;
    auto path = test::tempPath("nav.vkw").string();
    REQUIRE(saveScene(path, s, meshes(), textures(), error));
    Scene loaded;
    MeshLibrary lib = buildMeshLibrary();
    REQUIRE(loadScene(path, loaded, lib, textures(), error));
    CHECK_NEAR(loaded.navigation.cellSize, 0.25, 1e-6);
    CHECK_NEAR(loaded.navigation.agentRadius, 0.6, 1e-6);
    CHECK_NEAR(loaded.navigation.maxSlope, 30.0, 1e-6);
    CHECK_NEAR(loaded.navigation.agentHeight, 1.8, 1e-6);
}

// A NavAgent walks around the wall to its target, as a character
TEST(navigation_agent_in_game) {
    test::ImGuiContextGuard imgui;
    Scene s = level();
    Entity& goal = s.create("Goal");
    goal.transform.position[0] = -5;
    Entity& agent = s.create("Agent");
    agent.transform.position[0] = 5;
    Collider c;
    c.shape = Collider::Shape::Capsule;
    c.center = {0, 0.9f, 0};
    c.radius = 0.35f;
    c.height = 1.8f;
    agent.collider = c;
    std::unique_ptr<Behaviour> b = createBehaviour("NavAgent");
    REQUIRE(b);
    setField(*b, "target", std::string("Goal"));
    setField(*b, "speed", 5.f);
    setField(*b, "stopDistance", 0.5f);
    agent.behaviours.emplace_back(std::move(b));
    Game g;
    g.start(s, meshes());
    for (int i = 0; i < 60 * 10; ++i) {
        ImGui::NewFrame();
        g.step();
        ImGui::EndFrame();
    }
    Vec3 at = s.findByName("Agent")->transform.pos();
    CHECK(length(Vec3{at.x + 5.f, 0.f, at.z}) < 0.8f);   // arrived (stopping short by stopDistance at most)
    CHECK(at.y > -0.1f && at.y < 0.1f);
    CHECK(g.navigation().cellCount() > 0);
}

namespace {

// Two 10 m platforms with a 3 m gap between them (x -11..-1.5 and 1.5..11), and a link across it
Scene gapLevel(bool withLink) {
    Scene s;
    box(s, "Left", {-6.25f, -0.5f, 0}, {9.5f, 1, 10});
    box(s, "Right", {6.25f, -0.5f, 0}, {9.5f, 1, 10});
    if (withLink) {
        Entity& l = s.create("Jump");
        l.transform.position[0] = -2.f;
        l.navLink = NavLink{};
        l.navLink->end = {4.f, 0.f, 0.f};
    }
    return s;
}

}  // namespace

TEST(navigation_links) {
    Scene none = gapLevel(false);
    NavGrid nav;
    REQUIRE(nav.bake(none, meshes(), none.navigation));
    std::vector<Vec3> path;
    CHECK(!nav.findPath({-6, 0, 0}, {6, 0, 0}, path));
    Scene s = gapLevel(true);
    REQUIRE(nav.bake(s, meshes(), s.navigation));
    REQUIRE(nav.offMeshLinks().size() == 1);
    std::vector<uint8_t> jumps;
    REQUIRE(nav.findPath({-6, 0, 0}, {6, 0, 0}, path, &jumps));
    REQUIRE(jumps.size() == path.size());
    int hops = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i)
        if (jumps[i]) {
            ++hops;
            CHECK(path[i].x < -1.5f && path[i + 1].x > 1.5f);   // from one platform to the other
        }
    CHECK(hops == 1);
    REQUIRE(nav.findPath({6, 0, 0}, {-6, 0, 0}, path, &jumps));   // both ways
    s.findByName("Jump")->navLink->bothWays = false;
    REQUIRE(nav.bake(s, meshes(), s.navigation));
    CHECK(!nav.findPath({6, 0, 0}, {-6, 0, 0}, path));
    CHECK(nav.findPath({-6, 0, 0}, {6, 0, 0}, path));
}

TEST(navigation_obstacles_and_saved_bakes) {
    Scene s = level();
    NavGrid nav;
    REQUIRE(nav.bake(s, meshes(), s.navigation));
    std::vector<Vec3> path;
    REQUIRE(nav.findPath({5, 0.9f, 0}, {-5, 0.9f, 0}, path));
    nav.setObstacles({{{-0.5f, 0.f, 10.f}, {0.5f, 1.f, 12.f}}});   // a crate in the gap
    CHECK(nav.blocked({0.f, 0.f, 11.f}));
    CHECK(!nav.findPath({5, 0.9f, 0}, {-5, 0.9f, 0}, path));
    nav.setObstacles({});
    CHECK(nav.findPath({5, 0.9f, 0}, {-5, 0.9f, 0}, path));

    // Saved and loaded for the same level only
    auto file = test::tempPath("level.vkw.nav");
    REQUIRE(nav.save(file));
    NavLevel same = prepareNavLevel(s, meshes());
    NavGrid loaded;
    REQUIRE(loaded.load(file, same.hash));
    CHECK(loaded.cellCount() == nav.cellCount());
    std::vector<Vec3> again;
    REQUIRE(loaded.findPath({5, 0.9f, 0}, {-5, 0.9f, 0}, again));
    CHECK(again.size() == path.size());
    s.findByName("Wall B")->transform.position[2] += 1.f;   // the level changed
    CHECK(prepareNavLevel(s, meshes()).hash != same.hash);
    CHECK(!loaded.load(file, prepareNavLevel(s, meshes()).hash));
    Scene moved = level();
    Entity& crate = box(moved, "Crate", {3, 0.5f, 3}, {1, 1, 1});
    crate.rigidBody = RigidBody{};   // moving things aren't the level
    CHECK(prepareNavLevel(moved, meshes()).hash == same.hash);
}

// In the game: a saved bake is used; rigid bodies block; agents hop links and keep apart
TEST(navigation_in_game_obstacles_links_avoidance) {
    test::ImGuiContextGuard imgui;
    {   // a crate in the gap: no way through until it's gone
        Scene s = level();
        Entity& crate = box(s, "Crate", {0, 0.5f, 11}, {1, 1, 1});
        crate.rigidBody = RigidBody{};
        Game g;
        g.start(s, meshes());
        std::vector<Vec3> path;
        const NavGrid& nav = g.navigation();
        CHECK(nav.blocked({0.f, 0.f, 11.f}));
        CHECK(!nav.findPath({5, 0.9f, 0}, {-5, 0.9f, 0}, path));
    }
    {   // a saved bake next to the scene file is loaded instead of baking
        Scene s = level();
        std::string error, path = test::tempPath("navgame.vkw").string();
        REQUIRE(saveScene(path, s, meshes(), textures(), error));
        NavGrid baked;
        REQUIRE(baked.bake(s, meshes(), s.navigation));
        REQUIRE(baked.save(path + ".nav"));
        Game g;
        g.setScenePath(path);
        g.start(s, meshes());
        CHECK(g.navigation().cellCount() == baked.cellCount());
        CHECK(g.navigation().bakeMs() == 0.f);   // (loaded: no bake time)
    }
    {   // two agents hop the gap to the same target without walking into each other
        Scene s = gapLevel(true);
        Entity& goal = s.create("Goal");
        goal.transform.position[0] = 8.f;
        for (int k = 0; k < 2; ++k) {
            Entity& a = s.create(k ? "A2" : "A1");
            a.transform.position[0] = -7.f;
            a.transform.position[2] = k ? 0.3f : -0.3f;
            Collider c;
            c.shape = Collider::Shape::Capsule;
            c.center = {0, 0.9f, 0};
            c.radius = 0.3f;
            c.height = 1.8f;
            a.collider = c;
            std::unique_ptr<Behaviour> b = createBehaviour("NavAgent");
            setField(*b, "target", std::string("Goal"));
            setField(*b, "speed", 4.f);
            setField(*b, "stopDistance", 0.5f);
            a.behaviours.emplace_back(std::move(b));
        }
        Game g;
        g.start(s, meshes());
        float closest = 1e9f;
        for (int i = 0; i < 60 * 8; ++i) {
            ImGui::NewFrame();
            g.step();
            ImGui::EndFrame();
            Vec3 a = s.findByName("A1")->transform.pos(), b = s.findByName("A2")->transform.pos();
            closest = std::min(closest, length(Vec3{a.x - b.x, 0.f, a.z - b.z}));
        }
        for (const char* n : {"A1", "A2"}) {
            Vec3 p = s.findByName(n)->transform.pos();
            CHECK(p.x > 5.f);                   // across the gap
            CHECK(p.y > -0.2f && p.y < 0.3f);   // on the platform, not fallen in
        }
        CHECK(closest > 0.5f);
    }
}
