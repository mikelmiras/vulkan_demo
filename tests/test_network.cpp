// net/: UDP sockets and a server and a client game over the loopback: joining, players, replication, input,
// encryption and passwords, messages between machines
#include "test.h"

#include <algorithm>

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

namespace {

// Fires a ray along -Z (1 m up) on the fire action; the game state says what it hit ("hit": a name, or "miss")
struct Hitscan : BehaviourBase<Hitscan> {
    static constexpr const char* kTypeName = "TestNetHitscan";
    void update(GameContext& ctx, Entity& self) override {
        if (!ctx.input().pressed("fire")) return;
        RaycastHit hit;
        Vec3 from = ctx.worldPosition(self.id) + Vec3{0.f, 1.f, 0.f};
        Entity* e = ctx.raycast(from, {0, 0, -1}, 40.f, hit, self.id) ? ctx.find(hit.entity) : nullptr;
        ctx.state().setText("hit", e ? e->name : "miss");
    }
};

Scene bigScene(int statics, float relevance) {
    Scene s = netScene();
    for (int i = 0; i < statics; ++i) {
        Entity& e = s.create("Static " + std::to_string(i));
        e.transform.position[0] = (float)(i % 10) * 3.f - 15.f;
        e.transform.position[2] = (float)(i / 10) * 3.f - 6.f;
        e.replicated = Replicated{};
    }
    Entity& far = s.create("Far");
    far.transform.position[2] = -60.f;
    far.replicated = Replicated{};
    s.network.relevance = relevance;
    return s;
}

bool connect(Game& server, NetSession& sn, Game& client, NetSession& cn) {
    std::string error;
    if (!sn.host(server, 0, error) || !cn.join(client, "127.0.0.1:" + std::to_string(sn.port()), error)) return false;
    for (int i = 0; i < 60 && !cn.connected(); ++i) pump(server, sn, client, cn);
    return cn.connected();
}

}  // namespace

// Delta snapshots: once a client has the world, a still entity costs only its id; packets lost along the way don't
// stop the client following
TEST(net_delta_snapshots_and_loss) {
    test::ImGuiContextGuard imgui;
    Scene serverScene = bigScene(40, 0.f), clientScene = bigScene(40, 0.f);
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    REQUIRE(connect(server, sn, client, cn));
    for (int i = 0; i < 30; ++i) pump(server, sn, client, cn);
    uint64_t before = sn.bytesSent();
    for (int i = 0; i < 60; ++i) pump(server, sn, client, cn);   // a second of 20 snapshots
    uint64_t perSecond = sn.bytesSent() - before;
    // The 40 still entities' blobs (60+ bytes each) aren't sent again: 5 bytes each instead
    CHECK(perSecond < 20u * (40u * 12u + 600u));
    // With a quarter of the packets lost both ways (and some latency), the mover still follows the server
    sn.simulateNetwork(0.05f, 0.25f);
    cn.simulateNetwork(0.05f, 0.25f);
    for (int i = 0; i < 180; ++i) pump(server, sn, client, cn);
    const Entity* sm = serverScene.findByName("Mover");
    const Entity* cm = clientScene.findByName("Mover");
    REQUIRE(sm && cm);
    CHECK_NEAR(cm->transform.position[0], sm->transform.position[0], 0.6);
    CHECK(sm->transform.position[0] > 2.f);
    sn.stop();
    server.stop();
    client.stop();
}

// Interest: with a relevance distance, far entities aren't sent (hidden on the client) until they come near
TEST(net_interest_and_late_joiners) {
    test::ImGuiContextGuard imgui;
    Scene serverScene = bigScene(5, 20.f), clientScene = bigScene(5, 20.f);
    // Before the client joins, the server destroys two entities of the scene: one replicated, one not
    Entity& rock = serverScene.create("Rock");
    rock.transform.position[0] = 3.f;
    Entity& rockCopy = clientScene.create("Rock");
    rockCopy.transform.position[0] = 3.f;
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    std::string error;
    REQUIRE(sn.host(server, 0, error));
    server.context().destroy(serverScene.findByName("Static 2")->id);
    server.context().destroy(serverScene.findByName("Rock")->id);
    for (int i = 0; i < 10; ++i) pump(server, sn, client, cn);   // (the server runs on alone for a while)
    REQUIRE(cn.join(client, "127.0.0.1:" + std::to_string(sn.port()), error));
    for (int i = 0; i < 60 && !cn.connected(); ++i) pump(server, sn, client, cn);
    REQUIRE(cn.connected());
    for (int i = 0; i < 30; ++i) pump(server, sn, client, cn);
    CHECK(!clientScene.findByName("Static 2"));   // gone before it came: gone here too
    CHECK(!clientScene.findByName("Rock"));
    CHECK(clientScene.findByName("Static 1") && clientScene.findByName("Static 1")->visible);
    REQUIRE(clientScene.findByName("Far"));
    CHECK(!clientScene.findByName("Far")->visible);   // 60 m away: out of interest
    // The far one comes near: shown, where it is
    Entity* far = serverScene.findByName("Far");
    far->transform.position[2] = -5.f;
    for (int i = 0; i < 30; ++i) pump(server, sn, client, cn);
    CHECK(clientScene.findByName("Far")->visible);
    CHECK_NEAR(clientScene.findByName("Far")->transform.position[2], -5.0, 0.1);
    sn.stop();
    server.stop();
    client.stop();
}

// Prediction with latency: the client walks at once, ends where the server has it, and is put right (keeping the
// moves the server hasn't seen) when the server moves it
TEST(net_prediction_replays_inputs) {
    test::ImGuiContextGuard imgui;
    Scene serverScene = netScene(), clientScene = netScene();
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    REQUIRE(connect(server, sn, client, cn));
    sn.simulateNetwork(0.08f, 0.f);
    cn.simulateNetwork(0.08f, 0.f);
    for (int i = 0; i < 60; ++i) pump(server, sn, client, cn);
    // (the simulated latency both ways, plus a few frames: packets over real UDP may come an update late)
    CHECK(cn.roundTrip() > 0.15f && cn.roundTrip() < 0.3f);
    EntityId mine = cn.localPlayer();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
    float startClient = client.context().find(mine)->transform.position[2];
    pump(server, sn, client, cn);
    CHECK(client.context().find(mine)->transform.position[2] < startClient);   // moved at once, here
    for (int i = 0; i < 59; ++i) pump(server, sn, client, cn);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
    for (int i = 0; i < 40; ++i) pump(server, sn, client, cn);
    float c = client.context().find(mine)->transform.position[2], s = server.context().find(mine)->transform.position[2];
    CHECK_NEAR(c, s, 0.05);
    CHECK(s < startClient - 3.f);
    // (it predicted right, but for the odd input arriving late: the server held the previous one for a tick, and
    // the client was put right; a jittery machine (CI's) sees a few)
    uint64_t walking = cn.corrections();
    CHECK(walking <= 3);
    // The server moves it (a knock back): the client takes it, while walking on
    server.context().find(mine)->transform.position[0] += 4.f;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
    for (int i = 0; i < 30; ++i) pump(server, sn, client, cn);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
    for (int i = 0; i < 40; ++i) pump(server, sn, client, cn);
    CHECK(cn.corrections() > walking);   // (the knock back: put right)
    CHECK_NEAR(client.context().find(mine)->transform.position[0], server.context().find(mine)->transform.position[0], 0.05);
    CHECK_NEAR(client.context().find(mine)->transform.position[2], server.context().find(mine)->transform.position[2], 0.05);
    sn.stop();
    server.stop();
    client.stop();
}

// Lag compensation: a client fires when it sees a moving target in its sights; the server hits it only when it
// puts the target back where the client saw it
TEST(net_lag_compensation) {
    test::ImGuiContextGuard imgui;
    for (bool compensate : {true, false}) {
        auto scene = [] {
            Scene s = netScene();
            s.prefabs.back().entities[0].behaviours.emplace_back(std::make_unique<Hitscan>());
            Entity* mover = s.findByName("Mover");
            s.remove(mover->id);
            Entity& target = s.create("Target");   // a narrow post sliding along x, 10 m ahead
            target.transform.position[0] = -8.f;
            target.transform.position[2] = -10.f;
            target.collider = Collider{};
            target.collider->size = {0.4f, 4.f, 0.4f};
            target.collider->center = {0.f, 2.f, 0.f};
            target.replicated = Replicated{};
            auto slide = std::make_unique<Slider>();
            slide->speed = 4.f;
            target.behaviours.emplace_back(std::move(slide));
            s.network.spawnPoint = {0, 0, 0};
            s.network.spawnSpread = 0.f;
            return s;
        };
        Scene serverScene = scene(), clientScene = scene();
        Game server, client;
        server.start(serverScene, meshes());
        client.start(clientScene, meshes());
        NetSession sn, cn;
        REQUIRE(connect(server, sn, client, cn));
        sn.setLagCompensation(compensate);
        sn.simulateNetwork(0.1f, 0.f);
        cn.simulateNetwork(0.1f, 0.f);
        for (int i = 0; i < 40; ++i) pump(server, sn, client, cn);   // (the round trip measured)
        EntityId mine = cn.localPlayer();
        Vec3 me = client.context().worldPosition(mine);
        // Fire when the client sees the target right in front (its view lags the server's)
        bool fired = false;
        for (int i = 0; i < 400 && !fired; ++i) {
            if (clientScene.findByName("Target")->transform.position[0] >= me.x) {
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                fired = true;
            }
            pump(server, sn, client, cn);
        }
        REQUIRE(fired);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        for (int i = 0; i < 30; ++i) pump(server, sn, client, cn);
        CHECK(server.state().text("hit") == (compensate ? "Target" : "miss"));
        sn.stop();
        server.stop();
        client.stop();
    }
}

namespace {

// Passes everything between a client and a server (a man in the middle), keeping what went by
struct Proxy {
    net::UdpSocket socket;
    net::Address server, client;
    std::vector<std::vector<uint8_t>> seen;
    bool open(uint16_t serverPort) {
        std::string error;
        return socket.open(0, error) && net::resolve("127.0.0.1:" + std::to_string(serverPort), 1, server, error);
    }
    void pump() {
        uint8_t buf[2048];
        net::Address from;
        for (size_t n; (n = socket.receive(from, buf, sizeof(buf))) > 0;) {
            seen.emplace_back(buf, buf + n);
            if (from == server) {
                if (client.port) socket.send(client, buf, n);
            } else {
                client = from;
                socket.send(server, buf, n);
            }
        }
    }
};

bool contains(const std::vector<uint8_t>& bytes, const std::string& s) {
    return std::search(bytes.begin(), bytes.end(), s.begin(), s.end()) != bytes.end();
}

}  // namespace

// Sessions are encrypted (nothing readable on the way, plain or forged packets ignored) and a password keeps out
// players without it
TEST(net_sessions_encrypted_with_passwords) {
    test::ImGuiContextGuard imgui;
    Scene serverScene = netScene(), clientScene = netScene(), wrongScene = netScene();
    Game server, client, wrong;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    wrong.start(wrongScene, meshes());
    server.state().setText("motto", "attack at dawn");
    NetSession sn, cn, wn;
    std::string error;
    sn.setPassword("open sesame");
    REQUIRE(sn.host(server, 0, error));

    // The wrong password: turned away, no player made for it
    wn.setPassword("open says me");
    REQUIRE(wn.join(wrong, "127.0.0.1:" + std::to_string(sn.port()), error));
    for (int i = 0; i < 120 && !wn.refused(); ++i) pump(server, sn, wrong, wn);
    CHECK(wn.refused() && !wn.connected());
    CHECK(wn.status() == "Refused: wrong password");
    CHECK(sn.clients() == 0);
    wn.stop();

    // The right one, through a man in the middle: it plays, and nothing that went by can be read
    Proxy proxy;
    REQUIRE(proxy.open(sn.port()));
    cn.setPassword("open sesame");
    REQUIRE(cn.join(client, "127.0.0.1:" + std::to_string(proxy.socket.port()), error));
    for (int i = 0; i < 120 && !cn.connected(); ++i) {
        pump(server, sn, client, cn);
        proxy.pump();
    }
    REQUIRE(cn.connected());
    for (int i = 0; i < 60; ++i) {
        pump(server, sn, client, cn);
        proxy.pump();
    }
    CHECK(client.state().text("motto") == "attack at dawn");   // (it came, in a snapshot)
    CHECK(proxy.seen.size() > 50);
    bool readable = false;
    for (const auto& p : proxy.seen) readable |= contains(p, "attack at dawn") || contains(p, "Player") || contains(p, "player");
    CHECK(!readable);

    // Packets not sealed with its keys do nothing: a plain Hello, garbage dressed as sealed, a captured one again
    net::UdpSocket stranger;
    REQUIRE(stranger.open(0, error));
    net::Address to;
    REQUIRE(net::resolve("127.0.0.1:" + std::to_string(sn.port()), 1, to, error));
    std::vector<uint8_t> hello = {'V', 'K', 'N', '3', 1, 6, 0, 'p', 'l', 'a', 'y', 'e', 'r'};
    std::vector<uint8_t> forged = {'V', 'K', 'N', '3', 35};
    for (int i = 0; i < 60; ++i) forged.push_back((uint8_t)(i * 37));
    stranger.send(to, hello.data(), hello.size());
    stranger.send(to, forged.data(), forged.size());
    for (const auto& p : proxy.seen) stranger.send(to, p.data(), p.size());   // (replayed, from elsewhere)
    for (const auto& p : proxy.seen) proxy.socket.send(proxy.server, p.data(), p.size());   // (... and from its address)
    for (int i = 0; i < 30; ++i) {
        pump(server, sn, client, cn);
        proxy.pump();
    }
    CHECK(sn.clients() == 1);
    CHECK(cn.connected());
    cn.stop();
    sn.stop();
    server.stop();
    client.stop();
    wrong.stop();
}

namespace {

std::vector<std::string> g_serverGot, g_clientGot;

// Records the messages it gets (on which machine); the server answers each "ping" with a "pong" to its sender
struct Echo : BehaviourBase<Echo> {
    static constexpr const char* kTypeName = "TestNetEcho";
    void onMessage(GameContext& ctx, Entity& self, const Message& m) override {
        (ctx.isServer() ? g_serverGot : g_clientGot).push_back(m.name + std::to_string((int)m.value));
        if (ctx.isServer() && m.name == "ping") {
            Message pong{"pong", self.id, m.value, "", 0};
            ctx.sendToClients(self.id, pong, m.client);
        }
    }
};

}  // namespace

// Messages between behaviours on different machines: every one arrives, once, in order, through 30 % packet loss
TEST(net_messages_between_machines) {
    test::ImGuiContextGuard imgui;
    Scene serverScene = netScene(), clientScene = netScene();
    serverScene.findByName("Mover")->behaviours.emplace_back(std::make_unique<Echo>());
    clientScene.findByName("Mover")->behaviours.emplace_back(std::make_unique<Echo>());
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    REQUIRE(connect(server, sn, client, cn));
    CHECK(sn.isServer() && server.context().isServer() && client.context().isClient());
    EntityId mover = serverScene.findByName("Mover")->id;
    g_serverGot.clear();
    g_clientGot.clear();
    sn.simulateNetwork(0.03f, 0.3f);
    cn.simulateNetwork(0.03f, 0.3f);
    for (int i = 0; i < 40; ++i) {
        client.context().sendToServer(mover, Message{"ping", 0, (float)i, "", 0});
        if (i % 3 == 0) pump(server, sn, client, cn);
    }
    for (int i = 0; i < 400 && g_clientGot.size() < 40; ++i) pump(server, sn, client, cn);
    REQUIRE(g_serverGot.size() == 40);
    REQUIRE(g_clientGot.size() == 40);
    bool inOrder = true;
    for (int i = 0; i < 40; ++i)
        inOrder &= g_serverGot[i] == "ping" + std::to_string(i) && g_clientGot[i] == "pong" + std::to_string(i);
    CHECK(inOrder);
    // To all clients from the server: the host gets it too (it plays); a client can't send to clients
    server.context().sendToClients(mover, Message{"news", 0, 7.f, "", 0});
    client.context().sendToClients(mover, Message{"nope", 0, 0.f, "", 0});
    for (int i = 0; i < 120; ++i) pump(server, sn, client, cn);
    CHECK(g_clientGot.size() == 41 && g_clientGot.back() == "news7");
    CHECK(g_serverGot.size() == 41 && g_serverGot.back() == "news7");
    cn.stop();
    sn.stop();
    // Offline: both kinds are delivered here
    g_serverGot.clear();
    server.context().sendToServer(mover, Message{"solo", 0, 1.f, "", 0});
    server.context().sendToClients(mover, Message{"solo", 0, 2.f, "", 0});
    for (int i = 0; i < 3; ++i) pump(server, sn, client, cn);
    CHECK(g_serverGot.size() == 2);
    server.stop();
    client.stop();
}
