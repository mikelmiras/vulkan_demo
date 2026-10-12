// Multiplayer basics: one machine hosts (the server: it runs the game), others join (clients), over UDP.
//
//   - The server sends every client what's shared: entities with a Replicated component (where they are, whether
//     they're visible, their behaviours' fields; ones spawned and destroyed too) and the game state, 20 times a
//     second. Clients show those entities there instead of simulating them (smoothed between updates).
//   - Each client that joins gets a player, spawned from the scene's NetworkSettings::playerPrefab. Its input goes to
//     the server every update; on the server, that player's behaviours read it as their ctx.input().
//   - The client runs its own player too (prediction: it moves at once, without waiting for the server) and moves
//     it to where the server has it when they disagree by much.
//
// Both load the same scene (the host's start scene); a client joins once it has. Scene changes, save games and
// late-joiners' history aren't shared: this is the basics.
//
//   NetSession net;
//   net.host(game, 27015, error);           // or net.join(game, "192.168.1.5:27015", error)
//   every frame, after game.frame(): net.update(dt);
#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "game/game.h"
#include "net/udp.h"

class NetSession {
public:
    static constexpr uint16_t kDefaultPort = 27015;

    NetSession() = default;
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;
    ~NetSession() { stop(); }

    bool host(Game& game, uint16_t port, std::string& error);   // port 0: any free one (port() tells)
    bool join(Game& game, const std::string& address, std::string& error);   // "host[:port]"
    void stop();   // says goodbye; the game goes on alone
    // Once per rendered frame, after game.frame(): sends and receives, applies what came in
    void update(float dt);

    bool active() const { return socket_.isOpen(); }
    bool isServer() const { return server_; }
    bool connected() const { return server_ ? socket_.isOpen() : welcomed_; }   // (a client: once the server answered)
    uint16_t port() const { return socket_.port(); }
    int clients() const { return (int)clients_.size(); }
    EntityId localPlayer() const { return localPlayer_; }
    const std::string& status() const { return status_; }   // "Hosting on port 27015: 2 players", "Lost the server"

private:
    struct Input {
        uint32_t seq = 0;
        std::vector<float> values;   // the actions', in order
        float mouse[2] = {0, 0};
    };
    struct Client {   // (server side)
        uint16_t id = 0;
        net::Address address;
        std::string name;
        EntityId player = kNoEntity;
        InputMap input;
        std::deque<Input> pending;   // received, not yet applied (one per update)
        uint32_t received = 0, applied = 0;
        std::vector<float> last;     // held while no new input comes
        double heard = 0.0;          // when the last packet came
    };

    void serverReceive(const uint8_t* data, size_t size, const net::Address& from);
    void clientReceive(const uint8_t* data, size_t size);
    void sendSnapshots();
    void beforeTick();
    void afterTick();
    void dropClient(size_t index, const char* why);
    void applyEntity(class Reader& r, uint64_t tick);
    void smooth(float dt);
    void hookGame();

    Game* game_ = nullptr;
    net::UdpSocket socket_;
    bool server_ = false;
    double clock_ = 0.0;
    std::string status_;
    // Server
    std::vector<Client> clients_;
    uint16_t nextClient_ = 1;
    float snapshotTimer_ = 0.f;
    std::map<EntityId, int> gone_;   // destroyed replicated entities still being announced (snapshots left)
    std::vector<EntityId> sentLast_;
    std::map<EntityId, int> announced_;   // snapshots that carried how to make each entity
    uint64_t snapshotTick_ = 0;
    // Client
    net::Address serverAddress_;
    bool welcomed_ = false;
    uint16_t clientId_ = 0;
    EntityId localPlayer_ = kNoEntity;
    double helloAt_ = -1.0, heardAt_ = 0.0;
    uint32_t inputSeq_ = 0;
    std::deque<Input> sentInputs_;   // the last few (each packet carries them all: losses don't matter)
    uint64_t lastSnapshot_ = 0;
    struct Target {
        Vec3 position, scale;
        Quat rotation;
    };
    std::map<EntityId, Target> targets_;   // where the server has the entities this client shows
};
