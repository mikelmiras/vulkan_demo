// Gameplay framework: tags, messages between entities, timers and sequences, layers (collisions and queries)
#include "test.h"

#include "game/game.h"
#include "imgui.h"
#include "scene/serialization.h"

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

// Records the messages it gets; sends what it's told to on its first update
struct Mailbox : BehaviourBase<Mailbox> {
    static constexpr const char* kTypeName = "TestMailbox";
    std::vector<std::pair<int, Message>> got;   // (update number, message)
    int updates = 0;
    std::function<void(GameContext&, Entity&)> onFirst;
    std::function<void(GameContext&, Entity&, const Message&)> reply;
    void update(GameContext& ctx, Entity& self) override {
        if (++updates == 1 && onFirst) onFirst(ctx, self);
    }
    void onMessage(GameContext& ctx, Entity& self, const Message& m) override {
        got.emplace_back(updates, m);
        if (reply) reply(ctx, self, m);
    }
};

Mailbox& mailbox(Scene& s, const char* name) { return *s.findByName(name)->behaviour<Mailbox>(); }

Entity& withMailbox(Scene& s, const char* name, std::vector<std::string> tags = {}) {
    Entity& e = s.create(name);
    e.tags = std::move(tags);
    e.behaviours.emplace_back(std::make_unique<Mailbox>());
    return e;
}

Entity& box(Scene& s, const char* name, Vec3 pos, Vec3 size, int layer = 0) {
    Entity& e = s.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    e.transform.scale[0] = size.x; e.transform.scale[1] = size.y; e.transform.scale[2] = size.z;
    e.collider = Collider{};
    e.layer = (uint8_t)layer;
    return e;
}

}  // namespace

TEST(gameplay_tags) {
    test::ImGuiContextGuard imgui;
    Scene s;
    withMailbox(s, "A", {"Enemy"});
    withMailbox(s, "B", {"Friend"});
    withMailbox(s, "C", {"Enemy", "Boss"});
    std::vector<EntityId> found, foundSpawned;
    EntityId spawned = kNoEntity;
    Prefab p;
    p.name = "Minion";
    Entity m;
    m.id = 1;
    m.name = "Minion";
    m.tags = {"Enemy"};
    p.entities.push_back(m);
    s.addPrefab(p);
    mailbox(s, "B").onFirst = [&](GameContext& ctx, Entity&) {
        found = ctx.findWithTag("Enemy");
        spawned = ctx.spawn("Minion", {0, 0, 0});
        foundSpawned = ctx.findWithTag("Enemy");   // spawned this update: there too
        CHECK(ctx.findFirstWithTag("Boss") && ctx.findFirstWithTag("Boss")->name == "C");
        CHECK(!ctx.findFirstWithTag("Nobody"));
    };
    Game g;
    g.start(s, meshes());
    steps(g, 1);
    REQUIRE(found.size() == 2);
    CHECK(found[0] == s.findByName("A")->id && found[1] == s.findByName("C")->id);
    CHECK(foundSpawned.size() == 3 && foundSpawned[2] == spawned);

    // Saved with the scene, with the layer and the layer settings
    s.findByName("A")->layer = 5;
    s.layers.names[5] = "Enemies";
    s.layers.setMeet(5, 0, false);
    std::string error, path = test::tempPath("tags.vkw").string();
    REQUIRE(saveScene(path, s, meshes(), textures(), error));
    Scene loaded;
    MeshLibrary lib = buildMeshLibrary();
    REQUIRE(loadScene(path, loaded, lib, textures(), error));
    CHECK(loaded.findByName("C")->tags == std::vector<std::string>({"Enemy", "Boss"}));
    CHECK(loaded.findByName("A")->layer == 5);
    CHECK(loaded.layers.names[5] == "Enemies" && loaded.layers.find("Enemies") == 5);
    CHECK(!loaded.layers.meet(5, 0) && !loaded.layers.meet(0, 5) && loaded.layers.meet(5, 5) && loaded.layers.meet(0, 1));
}

TEST(gameplay_messages) {
    test::ImGuiContextGuard imgui;
    Scene s;
    withMailbox(s, "Sender");
    withMailbox(s, "Target", {"Team"});
    withMailbox(s, "Other", {"Team"});
    withMailbox(s, "Doomed");
    EntityId target = s.findByName("Target")->id, doomed = s.findByName("Doomed")->id;
    mailbox(s, "Sender").onFirst = [&](GameContext& ctx, Entity& self) {
        ctx.send(target, {"hit", self.id, 12.f, "sword"});
        ctx.sendToTag("Team", {"rally"});
        ctx.send(doomed, {"too late"});
        ctx.destroy(doomed);
        ctx.broadcast({"hello"});
    };
    mailbox(s, "Target").reply = [&](GameContext& ctx, Entity& self, const Message& m) {   // answers a hit
        if (m.name == "hit") ctx.send(m.from, {"ouch", self.id, m.value});
    };
    Game g;
    g.start(s, meshes());
    steps(g, 1);
    Mailbox& t = mailbox(s, "Target");
    REQUIRE(t.got.size() == 3);
    CHECK(t.got[0].second.name == "hit" && t.got[0].second.value == 12.f && t.got[0].second.text == "sword");
    CHECK(t.got[0].second.from == s.findByName("Sender")->id);
    CHECK(t.got[0].first == 1);   // in the same update, after the behaviours ran
    CHECK(t.got[1].second.name == "rally" && t.got[2].second.name == "hello");
    CHECK(mailbox(s, "Other").got.size() == 2);
    Mailbox& sender = mailbox(s, "Sender");
    REQUIRE(sender.got.size() == 2);   // hello, then the reply (sent while delivering: same update)
    CHECK(sender.got[1].second.name == "ouch" && sender.got[1].second.from == target && sender.got[1].first == 1);
    CHECK(s.find(doomed) == nullptr);
    // A message loop is cut off
    mailbox(s, "Other").reply = [&](GameContext& ctx, Entity& self, const Message&) { ctx.send(self.id, {"again"}); };
    mailbox(s, "Sender").onFirst = nullptr;
    mailbox(s, "Target").reply = nullptr;
    mailbox(s, "Target").onFirst = nullptr;
    mailbox(s, "Other").updates = 0;
    mailbox(s, "Other").onFirst = [&](GameContext& ctx, Entity& self) { ctx.send(self.id, {"start"}); };
    steps(g, 1);
    CHECK(mailbox(s, "Other").got.size() < 20);
}

TEST(gameplay_timers_and_sequences) {
    test::ImGuiContextGuard imgui;
    Scene s;
    withMailbox(s, "Clock");
    withMailbox(s, "Owner");
    EntityId owner = s.findByName("Owner")->id;
    std::vector<std::pair<std::string, float>> log;   // (what, game time)
    GameContext::TimerId cancelled = 0;
    int ticks = 0;
    mailbox(s, "Clock").onFirst = [&](GameContext& ctx, Entity&) {
        ctx.after(0.5f, [&](GameContext& c) { log.emplace_back("half", c.time()); });
        ctx.every(0.25f, [&](GameContext&) { ++ticks; });
        cancelled = ctx.after(0.3f, [&](GameContext& c) { log.emplace_back("cancelled", c.time()); });
        ctx.after(0.4f, [&](GameContext& c) { log.emplace_back("owned", c.time()); }, owner);
        ctx.sequence().wait(0.1f).then([&](GameContext& c) { log.emplace_back("one", c.time()); })
            .wait(0.1f).then([&](GameContext& c) {
                log.emplace_back("two", c.time());
                c.after(0.f, [&](GameContext& c2) { log.emplace_back("nested", c2.time()); });   // next update
            });
    };
    mailbox(s, "Owner").onFirst = [&](GameContext& ctx, Entity&) { ctx.cancel(cancelled); };
    Game g;
    g.start(s, meshes());
    steps(g, 12);   // 0.2 s
    steps(g, 6);    // 0.3 s
    REQUIRE(s.find(owner));
    s.remove(std::vector<EntityId>{owner});   // gone before 0.4 s: its timer goes with it
    steps(g, 42);   // 1.0 s
    auto at = [&](const std::string& what) {
        for (const auto& l : log)
            if (l.first == what) return l.second;
        return -1.f;
    };
    CHECK_NEAR(at("one"), 0.1, 1.0 / 60.0 + 1e-4);
    CHECK_NEAR(at("two"), 0.2, 1.0 / 60.0 + 1e-4);
    CHECK_NEAR(at("nested"), at("two") + 1.0 / 60.0, 1e-4);
    CHECK_NEAR(at("half"), 0.5, 1.0 / 60.0 + 1e-4);
    CHECK(at("cancelled") < 0.f);
    CHECK(at("owned") < 0.f);
    CHECK(ticks == 3);   // set at 1/60 s: 0.27, 0.52, 0.77 (1.02 is past the end)
    // Order: one, two, nested, half
    REQUIRE(log.size() == 4);
    CHECK(log[0].first == "one" && log[3].first == "half");
}

// Layers: colliders meet only as the matrix says; queries see only the layers asked for
TEST(gameplay_layers) {
    test::ImGuiContextGuard imgui;
    Scene s;
    s.layers.names[1] = "Ghosts";
    s.layers.names[2] = "Sensors";
    s.layers.setMeet(1, 0, false);   // ghosts pass through the default layer
    s.layers.setMeet(1, 2, false);
    box(s, "Floor", {0, -0.5f, 0}, {20, 1, 20});
    for (int ghost = 0; ghost < 2; ++ghost) {
        Entity& b = box(s, ghost ? "Ghost" : "Crate", {ghost ? 3.f : -3.f, 3.f, 0}, {1, 1, 1}, ghost);
        b.rigidBody = RigidBody{};
    }
    Entity& trigger = box(s, "Zone", {0, 1.f, 5}, {2, 2, 2}, 2);
    trigger.collider->trigger = true;
    Game g;
    g.start(s, meshes());
    steps(g, 90);
    CHECK_NEAR(s.findByName("Crate")->transform.position[1], 0.5, 0.05);   // on the floor
    CHECK(s.findByName("Ghost")->transform.position[1] < -2.f);          // through it

    const Physics& p = g.physics();
    RaycastHit hit;
    REQUIRE(p.raycast({-3, 10, 0}, {0, -1, 0}, 20.f, hit));
    CHECK(hit.entity == s.findByName("Crate")->id);
    REQUIRE(p.raycast({-3, 10, 0}, {0, -1, 0}, 20.f, hit, kNoEntity, 1u << 0));
    uint32_t onlyGhosts = 1u << 1;
    CHECK(!p.raycast({-3, 10, 0}, {0, -1, 0}, 20.f, hit, kNoEntity, onlyGhosts));
    CHECK(p.overlapSphere({-3, 0.5f, 0}, 0.2f, true, kNoEntity, onlyGhosts).empty());
    CHECK(!p.overlapSphere({-3, 0.5f, 0}, 0.2f).empty());

    // The trigger (Sensors) notices a crate but not a ghost
    s.findByName("Crate")->transform.position[0] = 0.f;
    s.findByName("Crate")->transform.position[1] = 1.f;
    s.findByName("Crate")->transform.position[2] = 5.f;
    s.findByName("Crate")->rigidBody->useGravity = false;
    s.findByName("Ghost")->transform.position[0] = 0.5f;
    s.findByName("Ghost")->transform.position[1] = 1.f;
    s.findByName("Ghost")->transform.position[2] = 5.f;
    s.findByName("Ghost")->rigidBody->useGravity = false;
    s.findByName("Ghost")->rigidBody->velocity = {};
    steps(g, 1);
    std::vector<std::pair<EntityId, EntityId>> pairs = g.physics().triggerOverlaps(s);
    bool crate = false, ghost = false;
    for (const auto& [t, o] : pairs) {
        crate = crate || o == s.findByName("Crate")->id;
        ghost = ghost || o == s.findByName("Ghost")->id;
    }
    CHECK(crate);
    CHECK(!ghost);
}
