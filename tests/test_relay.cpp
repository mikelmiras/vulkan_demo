// net/relay: the relay's protocol, lobbies listed and joined by code over the loopback (straight through, and
// forwarded when the NATs let nothing through), dedicated servers, and multiplayer from the game's menus
#include "test.h"

#include <set>

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
    for (int i = 0; i < 5; ++i) w.pump(server, sn, client, cn);
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
