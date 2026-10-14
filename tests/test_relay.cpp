// net/relay: the relay's protocol, lobbies listed and joined by code over the loopback (straight through, and
// forwarded when the NATs let nothing through), dedicated servers, multiplayer from the game's menus, the relay's
// secret, private lobbies and rate limits
#include "test.h"

#include <chrono>
#include <set>
#include <thread>

#include "game/game.h"
#include "game/ui_layout.h"
#include "imgui.h"
#include "net/net_session.h"
#include "net/relay.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

Scene lobbyScene() {
    Scene s;
    Entity& mover = s.create("Mover");
    mover.replicated = Replicated{};
    Prefab p;
    p.name = "Player";
    Entity root;
    root.id = 1;
    root.name = "Player";
    root.replicated = Replicated{};
    p.entities.push_back(std::move(root));
    s.addPrefab(p);
    s.network.playerPrefab = "Player";
    return s;
}

// One frame of the relay and both games (the relay's clock: the frames')
struct World {
    relay::Server relay;
    double now = 0.0;
    std::string address() const { return "127.0.0.1:" + std::to_string(relay.port()); }
    void pump(Game& server, NetSession& sn, Game& client, NetSession& cn) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        client.frame(1.f / 60.f, false, false);
        cn.update(1.f / 60.f);
        relay.update(now);
        server.frame(1.f / 60.f, false, false);
        sn.update(1.f / 60.f);
        relay.update(now);
        ImGui::EndFrame();
        now += 1.0 / 60.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // (real time for the loopback: macOS takes a moment)
    }
};

}  // namespace

TEST(relay_protocol) {
    relay::Message m;
    m.type = relay::Type::Register;
    m.token = 77;
    m.game = "Sample Game";
    m.name = "Mikel's room";
    m.players = 3;
    m.maxPlayers = 8;
    std::vector<uint8_t> bytes = relay::encode(m);
    CHECK(relay::isRelayPacket(bytes.data(), bytes.size()));
    relay::Message back;
    REQUIRE(relay::decode(bytes.data(), bytes.size(), back));
    CHECK(back.type == relay::Type::Register && back.token == 77 && back.game == "Sample Game" && back.name == "Mikel's room");
    CHECK(back.players == 3 && back.maxPlayers == 8);
    m = {};
    m.type = relay::Type::Lobbies;
    m.lobbies = {{"ABC234", "One", 1, 4}, {"XYZ789", "Two", 2, 2}};
    bytes = relay::encode(m);
    REQUIRE(relay::decode(bytes.data(), bytes.size(), back));
    REQUIRE(back.lobbies.size() == 2);
    CHECK(back.lobbies[1].code == "XYZ789" && back.lobbies[1].name == "Two" && back.lobbies[1].maxPlayers == 2);
    m = {};
    m.type = relay::Type::Forward;
    m.address = {0x7F000001u, 4242};
    m.payload = {'V', 'K', 'N', '2', 9};
    bytes = relay::encode(m);
    REQUIRE(relay::decode(bytes.data(), bytes.size(), back));
    CHECK(back.address == m.address && back.payload == m.payload);
    m = {};
    m.type = relay::Type::Peer;
    m.code = "K7QX2M";
    m.address = {0x0A000002u, 5};
    m.host = true;
    bytes = relay::encode(m);
    REQUIRE(relay::decode(bytes.data(), bytes.size(), back));
    CHECK(back.code == "K7QX2M" && back.host && back.address.port == 5);
    bytes.resize(bytes.size() - 3);   // (cut short: refused)
    CHECK(!relay::decode(bytes.data(), bytes.size(), back));
    const uint8_t game[] = {'V', 'K', 'N', '2', 1};
    CHECK(!relay::isRelayPacket(game, sizeof(game)));
    std::mt19937 rng(5);
    std::set<std::string> codes;
    for (int i = 0; i < 200; ++i) {
        std::string c = relay::makeCode(rng);
        CHECK(c.size() == 6 && c.find_first_of("01OI") == std::string::npos);
        codes.insert(c);
    }
    CHECK(codes.size() > 190);
}

// A host lists its game on the relay; a player finds it, joins by its code straight through (the punch opens the
// way); the lobby's player count follows; an unknown code fails; the lobby goes when the host stops
TEST(relay_lobbies_joined_by_code) {
    test::ImGuiContextGuard imgui;
    World w;
    std::string error;
    REQUIRE(w.relay.open(0, error));
    Scene serverScene = lobbyScene(), clientScene = lobbyScene();
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    REQUIRE(sn.host(server, 0, error));
    REQUIRE(sn.advertise(w.address(), "Test Game", "Room one", 4, error));
    for (int i = 0; i < 60 && sn.lobbyCode().empty(); ++i) w.pump(server, sn, client, cn);
    REQUIRE(sn.lobbyCode().size() == 6);
    CHECK(w.relay.lobbyCount() == 1);
    CHECK(sn.publicAddress().ip == 0x7F000001u && sn.publicAddress().port == sn.port());
    CHECK(sn.status().find(sn.lobbyCode()) != std::string::npos);

    relay::LobbyQuery query;
    REQUIRE(query.start(w.address(), "Test Game", error));
    for (int i = 0; i < 100 && !query.done(); ++i) {
        query.update(1.f / 60.f);
        w.relay.update(w.now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(query.done() && query.error().empty());
    REQUIRE(query.lobbies().size() == 1);
    CHECK(query.lobbies()[0].code == sn.lobbyCode() && query.lobbies()[0].name == "Room one");
    CHECK(query.lobbies()[0].players == 1 && query.lobbies()[0].maxPlayers == 4);   // (the host plays)
    relay::LobbyQuery other;   // (another game's: none)
    REQUIRE(other.start(w.address(), "Other Game", error));
    for (int i = 0; i < 100 && !other.done(); ++i) {
        other.update(1.f / 60.f);
        w.relay.update(w.now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(other.done() && other.lobbies().empty());

    std::string code = sn.lobbyCode();
    for (char& c : code) c = (char)std::tolower((unsigned char)c);   // (typed in lower case: still found)
    REQUIRE(cn.joinLobby(client, w.address(), code, error));
    for (int i = 0; i < 120 && !cn.connected(); ++i) w.pump(server, sn, client, cn);
    REQUIRE(cn.connected());
    CHECK(!cn.throughRelay());
    CHECK(w.relay.forwarded() == 0);   // (straight through: the relay only introduced them)
    CHECK(sn.clients() == 1 && cn.localPlayer() != kNoEntity);
    for (int i = 0; i < 180; ++i) w.pump(server, sn, client, cn);   // (the host registers again: 2 players now)
    REQUIRE(query.start(w.address(), "Test Game", error));
    for (int i = 0; i < 100 && !query.done(); ++i) {
        query.update(1.f / 60.f);
        w.relay.update(w.now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(query.lobbies().size() == 1);
    CHECK(query.lobbies()[0].players == 2);

    NetSession lost;   // a code nobody hosts
    Scene lostScene = lobbyScene();
    Game lostGame;
    lostGame.start(lostScene, meshes());
    REQUIRE(lost.joinLobby(lostGame, w.address(), "ZZZZZZ", error));
    for (int i = 0; i < 60 && !lost.lobbyFailed(); ++i) w.pump(server, sn, lostGame, lost);
    CHECK(lost.lobbyFailed() && !lost.connected());
    CHECK(lost.status() == "No lobby ZZZZZZ");
    lost.stop();
    lostGame.stop();

    cn.stop();
    sn.stop();   // (unregisters: gone at once)
    for (int i = 0; i < 5; ++i) w.relay.update(w.now);
    CHECK(w.relay.lobbyCount() == 0);
    client.stop();
    server.stop();
}

// NATs that let nothing through directly: the player gives up on the direct way and everything goes through the
// relay, both ways, and the game plays as usual
TEST(relay_forwards_when_nat_blocks) {
    test::ImGuiContextGuard imgui;
    World w;
    std::string error;
    REQUIRE(w.relay.open(0, error));
    Scene serverScene = lobbyScene(), clientScene = lobbyScene();
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    sn.simulateStrictNat(true);
    cn.simulateStrictNat(true);
    REQUIRE(sn.host(server, 0, error));
    REQUIRE(sn.advertise(w.address(), "Test Game", "Behind a NAT", 8, error));
    for (int i = 0; i < 60 && sn.lobbyCode().empty(); ++i) w.pump(server, sn, client, cn);
    REQUIRE(!sn.lobbyCode().empty());
    REQUIRE(cn.joinLobby(client, w.address(), sn.lobbyCode(), error));
    for (int i = 0; i < 100; ++i) w.pump(server, sn, client, cn);   // (under 2 s: still trying the direct way)
    CHECK(!cn.connected());
    for (int i = 0; i < 200 && !cn.connected(); ++i) w.pump(server, sn, client, cn);
    REQUIRE(cn.connected());
    CHECK(cn.throughRelay());
    CHECK(cn.status().find("through the relay") != std::string::npos);
    for (int i = 0; i < 60; ++i) w.pump(server, sn, client, cn);
    CHECK(w.relay.forwarded() > 50);   // (inputs one way, snapshots the other)
    EntityId mine = cn.localPlayer();
    REQUIRE(mine != kNoEntity);
    CHECK(client.context().find(mine) && server.context().find(mine));
    CHECK(client.context().find(sn.localPlayer()));   // (the host's player, replicated through the relay)
    cn.stop();
    for (int i = 0; i < 60 && sn.clients() > 0; ++i) w.pump(server, sn, client, cn);   // (loopback may take a moment)
    CHECK(sn.clients() == 0);   // (its goodbye came through the relay too)
    sn.stop();
    client.stop();
    server.stop();
}

// A dedicated server has no player of its own; players joining get theirs
TEST(net_dedicated_server) {
    test::ImGuiContextGuard imgui;
    World w;
    Scene serverScene = lobbyScene(), clientScene = lobbyScene();
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    std::string error;
    REQUIRE(sn.host(server, 0, error, true));
    CHECK(sn.dedicated() && sn.localPlayer() == kNoEntity);
    int players = 0;
    for (const Entity& e : serverScene.entities) players += e.name == "Player";
    CHECK(players == 0);
    REQUIRE(cn.join(client, "127.0.0.1:" + std::to_string(sn.port()), error));
    for (int i = 0; i < 60 && !cn.connected(); ++i) w.pump(server, sn, client, cn);
    REQUIRE(cn.connected());
    CHECK(cn.localPlayer() != kNoEntity);
    CHECK(sn.status().rfind("Serving on port", 0) == 0);
    cn.stop();
    sn.stop();
    client.stop();
    server.stop();
}

// The game's menus: "@host", "@joinlobby {code}" and the rest go to the network handler, formatted
TEST(net_ui_actions) {
    test::ImGuiContextGuard imgui;
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    Scene s;
    Entity& menu = s.create("Menu");
    menu.ui = UIElement{};
    menu.ui->kind = UIElement::Kind::List;
    menu.ui->anchorMin[0] = menu.ui->anchorMin[1] = menu.ui->anchorMax[0] = menu.ui->anchorMax[1] = 0.5f;
    menu.ui->offsetMin[0] = -200, menu.ui->offsetMin[1] = -100, menu.ui->offsetMax[0] = 200, menu.ui->offsetMax[1] = 100;
    Entity& join = s.create("Join", menu.id);
    join.ui = UIElement{};
    join.ui->kind = UIElement::Kind::Button;
    join.ui->offsetMax[1] = 60;
    join.ui->action = "@joinlobby {typed}; @lobbies";
    Game g;
    g.start(s, meshes());
    std::vector<std::pair<std::string, std::string>> asked;
    g.setNetworkHandler([&](const std::string& verb, const std::string& arg) { asked.push_back({verb, arg}); });
    g.state().set("typed", json::Value("K7QX2M"));
    GameUI ui;
    auto frame = [&] {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, 1920.f, 1080.f, true);
        g.drawUI(ui);
        ui.end();
        ImGui::EndFrame();
    };
    auto tap = [&](ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        frame();
    };
    frame();
    tap(ImGuiKey_DownArrow);   // (wakes the menu: Join)
    tap(ImGuiKey_Enter);
    REQUIRE(asked.size() == 2);
    CHECK(asked[0].first == "joinlobby" && asked[0].second == "K7QX2M");
    CHECK(asked[1].first == "lobbies" && asked[1].second.empty());
    g.stop();
}

namespace {

std::vector<relay::Lobby> listLobbies(World& w, const std::string& game) {
    relay::LobbyQuery q;
    std::string error;
    if (!q.start(w.address(), game, error)) return {};
    for (int i = 0; i < 100 && !q.done(); ++i) {
        q.update(1.f / 60.f);
        w.relay.update(w.now);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return q.lobbies();
}

}  // namespace

// A relay with a secret takes lobbies only from hosts that know it; private lobbies aren't listed but are joined
// by code (with the game's password); lobbies with one say so
TEST(relay_secret_and_private_lobbies) {
    test::ImGuiContextGuard imgui;
    World w;
    std::string error;
    w.relay.setSecret("relay key");
    REQUIRE(w.relay.open(0, error));
    Scene serverScene = lobbyScene(), clientScene = lobbyScene();
    Game server, client;
    server.start(serverScene, meshes());
    client.start(clientScene, meshes());
    NetSession sn, cn;
    for (const char* secret : {"", "wrong key"}) {   // without it, or with another: no lobby
        sn.setRelaySecret(secret);
        REQUIRE(sn.host(server, 0, error));
        REQUIRE(sn.advertise(w.address(), "Test Game", "Room", 4, error));
        for (int i = 0; i < 90 && !sn.relayRefused(); ++i) w.pump(server, sn, client, cn);
        CHECK(sn.relayRefused() && sn.lobbyCode().empty());
        CHECK(w.relay.lobbyCount() == 0);
        sn.stop();
    }
    // Knowing it: a private lobby with a password
    sn.setRelaySecret("relay key");
    sn.setPassword("pw");
    REQUIRE(sn.host(server, 0, error));
    REQUIRE(sn.advertise(w.address(), "Test Game", "Hidden room", 4, error, false));
    for (int i = 0; i < 90 && sn.lobbyCode().empty(); ++i) w.pump(server, sn, client, cn);
    REQUIRE(sn.lobbyCode().size() == 6);
    CHECK(!sn.relayRefused());
    CHECK(listLobbies(w, "Test Game").empty());
    cn.setPassword("pw");
    REQUIRE(cn.joinLobby(client, w.address(), sn.lobbyCode(), error));
    for (int i = 0; i < 120 && !cn.connected(); ++i) w.pump(server, sn, client, cn);
    CHECK(cn.connected());
    cn.stop();
    sn.stop();
    // Listed, with a password: the list says so
    REQUIRE(sn.host(server, 0, error));
    REQUIRE(sn.advertise(w.address(), "Test Game", "Locked room", 4, error));
    for (int i = 0; i < 90 && sn.lobbyCode().empty(); ++i) w.pump(server, sn, client, cn);
    std::vector<relay::Lobby> listed = listLobbies(w, "Test Game");
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "Locked room" && listed[0].password);
    sn.stop();
    client.stop();
    server.stop();
}

// Rate limits: a flood from one ip gets only its burst answered; the rest is dropped (and refills with time)
TEST(relay_rate_limits) {
    relay::Server relay;
    relay.limits.packetsPerSecond = 10.0;
    relay.limits.packetBurst = 20.0;
    std::string error;
    REQUIRE(relay.open(0, error));
    net::UdpSocket flood;
    REQUIRE(flood.open(0, error));
    net::Address to;
    REQUIRE(net::resolve("127.0.0.1:" + std::to_string(relay.port()), 1, to, error));
    relay::Message list;
    list.type = relay::Type::List;
    list.game = "Test Game";
    std::vector<uint8_t> bytes = relay::encode(list);
    auto answers = [&](int sent, double now) {
        for (int i = 0; i < sent; ++i) flood.send(to, bytes.data(), bytes.size());
        int got = 0;
        uint8_t buf[2048];
        net::Address from;
        for (int k = 0; k < 200; ++k) {
            relay.update(now);
            for (size_t n; (n = flood.receive(from, buf, sizeof(buf))) > 0;) ++got;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return got;
    };
    CHECK(answers(100, 0.0) == 20);
    CHECK(relay.dropped() == 80);
    CHECK(answers(100, 1.0) == 10);   // (a second later: ten more)
    // Lobbies per ip are limited too
    relay.limits.packetsPerSecond = 1e6;
    relay.limits.packetBurst = 1e6;
    relay.limits.lobbies = 3;
    for (uint32_t t = 1; t <= 6; ++t) {
        relay::Message r;
        r.type = relay::Type::Register;
        r.token = t;
        r.game = "Test Game";
        r.name = "Room";
        std::vector<uint8_t> b = relay::encode(r);
        flood.send(to, b.data(), b.size());
    }
    answers(0, 2.0);
    CHECK(relay.lobbyCount() == 3);
}
