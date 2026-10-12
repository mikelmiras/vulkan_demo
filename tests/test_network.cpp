// net/: UDP sockets and a server and a client game over the loopback: joining, players, replication, input
#include "test.h"

#include "game/game.h"
#include "imgui.h"
#include "net/net_session.h"
#include "net/udp.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

// Walks forward (-Z) while "move_forward" is down, 5 m/s
struct Walker : BehaviourBase<Walker> {
    static constexpr const char* kTypeName = "TestNetWalker";
    void update(GameContext& ctx, Entity& self) override {
        if (ctx.input().down("move_forward")) self.transform.position[2] -= 5.f * ctx.dt();
    }
};

// Slides along +X, 1 m/s, and counts the updates in the game state
struct Slider : BehaviourBase<Slider> {
    static constexpr const char* kTypeName = "TestNetSlider";
    float speed = 1.f;
    void fields(FieldVisitor& v) override { v.field("speed", speed); }
    void update(GameContext& ctx, Entity& self) override {
        self.transform.position[0] += speed * ctx.dt();
        ctx.state().add("ticks", 1.f);
    }
};

Scene netScene() {
    Scene s;
    Entity& mover = s.create("Mover");
    mover.replicated = Replicated{};
    mover.behaviours.emplace_back(std::make_unique<Slider>());
    Prefab p;
    p.name = "Player";
    Entity root;
    root.id = 1;
    root.name = "Player";
    root.replicated = Replicated{};
    root.behaviours.emplace_back(std::make_unique<Walker>());
    p.entities.push_back(std::move(root));
    s.addPrefab(p);
    s.network.playerPrefab = "Player";
    s.network.spawnPoint = {0, 0, 0};
    return s;
}

// One frame of both: the client reads the keyboard, the server doesn't
void pump(Game& server, NetSession& sn, Game& client, NetSession& cn) {
    ImGui::GetIO().DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    client.frame(1.f / 60.f, true, true);
    cn.update(1.f / 60.f);
    server.frame(1.f / 60.f, false, false);
    sn.update(1.f / 60.f);
    ImGui::EndFrame();
}

}  // namespace

TEST(net_udp_loopback) {
    net::UdpSocket a, b;
    std::string error;
    REQUIRE(a.open(0, error));
    REQUIRE(b.open(0, error));
    CHECK(a.port() != 0 && a.port() != b.port());
    net::Address to;
    REQUIRE(net::resolve("127.0.0.1:" + std::to_string(b.port()), 1, to, error));
    CHECK(to.ip == 0x7F000001u && to.port == b.port());
    REQUIRE(a.send(to, "hello", 5));
    char buf[16];
    net::Address from;
    size_t n = 0;
    for (int i = 0; i < 200 && n == 0; ++i) n = b.receive(from, buf, sizeof(buf));
    REQUIRE(n == 5);
    CHECK(std::string(buf, 5) == "hello");
    CHECK(from.port == a.port());
    CHECK(b.receive(from, buf, sizeof(buf)) == 0);   // (nothing more: doesn't wait)
    net::Address dflt;
    REQUIRE(net::resolve("localhost", 27015, dflt, error));
    CHECK(dflt.port == 27015);
    CHECK(!net::resolve("127.0.0.1:99999", 1, dflt, error));
}

TEST(net_server_and_client) {
    test::ImGuiContextGuard imgui;
    Scene serverScene = netScene(), clientScene = netScene();
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    std::string error;
    REQUIRE(sn.host(server, 0, error));
    REQUIRE(cn.join(client, "127.0.0.1:" + std::to_string(sn.port()), error));
    for (int i = 0; i < 30 && !cn.connected(); ++i) pump(server, sn, client, cn);
    REQUIRE(cn.connected());
    CHECK(sn.clients() == 1);
    EntityId mine = cn.localPlayer();
    REQUIRE(mine != kNoEntity);
    CHECK(sn.localPlayer() != kNoEntity && sn.localPlayer() != mine);   // (the host plays too)
    for (int i = 0; i < 30; ++i) pump(server, sn, client, cn);

    // The client shows the server's mover (not simulating it), and the game state
    const Entity* sm = serverScene.findByName("Mover");
    const Entity* cm = clientScene.findByName("Mover");
    REQUIRE(sm && cm);
    CHECK(!client.simulated(cm->id));
    CHECK(sm->transform.position[0] > 0.5f);
    CHECK_NEAR(cm->transform.position[0], sm->transform.position[0], 0.2);
    CHECK(client.state().number("ticks") > 10.f);
    // Both players exist on the client, made with the server's ids; only its own is simulated there
    REQUIRE(client.context().find(mine));
    REQUIRE(client.context().find(sn.localPlayer()));
    CHECK(client.simulated(mine));
    CHECK(!client.simulated(sn.localPlayer()));
    CHECK(client.localPlayer() == mine);

    // Holding W on the client walks its player, there at once and on the server (the host's player stays put)
    float startServer = server.context().find(mine)->transform.position[2];
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
    for (int i = 0; i < 60; ++i) pump(server, sn, client, cn);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
    for (int i = 0; i < 20; ++i) pump(server, sn, client, cn);
    float walked = startServer - server.context().find(mine)->transform.position[2];
    CHECK(walked > 3.f && walked < 7.f);   // about a second at 5 m/s
    CHECK_NEAR(client.context().find(mine)->transform.position[2], server.context().find(mine)->transform.position[2], 1.f);
    CHECK_NEAR(server.context().find(sn.localPlayer())->transform.position[2], 0.f, 1e-4);

    // Behaviour fields follow: the server changes the mover's speed
    setField(*serverScene.findByName("Mover")->behaviours[0], "speed", 3.f);
    for (int i = 0; i < 10; ++i) pump(server, sn, client, cn);
    float clientSpeed = 0.f;
    struct Get : FieldVisitor {
        float* out;
        void field(const char* n, float& v, float, float, float) override { if (std::string(n) == "speed") *out = v; }
        void field(const char*, bool&) override {}
        void field(const char*, Vec3&, float) override {}
        void field(const char*, std::string&) override {}
    } get;
    get.out = &clientSpeed;
    clientScene.findByName("Mover")->behaviours[0]->fields(get);
    CHECK(clientSpeed == 3.f);

    // The client leaves: its player goes, on the server and (announced) nowhere else
    cn.stop();
    for (int i = 0; i < 10; ++i) pump(server, sn, client, cn);
    CHECK(sn.clients() == 0);
    CHECK(!server.context().find(mine));
    sn.stop();
    server.stop();
    client.stop();
}
