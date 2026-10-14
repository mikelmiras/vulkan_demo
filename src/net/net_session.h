// Multiplayer: one machine hosts (the server: it runs the game), others join (clients), over UDP.
//
//   - The server sends every client what's shared: entities with a Replicated component (where they are, whether
//     they're visible, their behaviours' fields; ones spawned and destroyed too) and the game state, 20 times a
//     second. Clients show those entities there instead of simulating them (smoothed between updates).
//   - Snapshots are delta-compressed: each client acknowledges the snapshots it got whole, and the next one only
//     carries what changed since the last it acknowledged (an unchanged entity costs its id). A snapshot is split
//     into packets as needed and used only once all of them have arrived.
//   - Interest: with NetworkSettings::relevance set, a client only gets the entities within that distance of its
//     player (others are hidden on it until they come near again).
//   - Each client that joins gets a player, spawned from the scene's NetworkSettings::playerPrefab. Its input goes to
//     the server every update; on the server, that player's behaviours read it as their ctx.input().
//   - The client runs its own player too (prediction: it moves at once). When a snapshot says where the server has
//     it after the client's input n, the client takes that and replays the moves of its inputs after n (which the
//     server hasn't seen yet): corrections without the player jumping back.
//   - Lag compensation: while a remote player's behaviours update on the server, the other replicated entities with
//     colliders are put back where that player saw them (its round trip, the time its input waited, and the
//     client's smoothing ago), so its rays and overlaps hit what it aimed at.
//   - Late joiners get the scene the server is in (loaded before they play) and every replicated or scene entity
//     destroyed before they came (until they acknowledge it).
//
//   - Every session is encrypted (net/crypto.h): the client offers an X25519 key, the server answers with its own,
//     and from then on every packet either way is sealed with ChaCha20-Poly1305 (keys each way from the exchange;
//     a counter per packet: changed, forged and replayed packets are dropped). A game's password goes into the
//     keys too: a client with the wrong one can't open anything and is turned away.
//   - Messages between behaviours on different machines (GameContext::sendToServer / sendToClients): numbered,
//     resent until acknowledged, delivered in order, as onMessage() there.
//   - Dedicated servers (vkgame --server): a host with no player of its own, nothing drawn.
//   - Lobbies (net/relay.h): a host can advertise its game on a relay server and gets a code; players join by the
//     code: the relay introduces them, they punch through their NATs to each other, and when no packet gets
//     through directly in a couple of seconds, everything between them goes through the relay instead. A lobby can
//     be private (not listed: joined by its code) and say it has a password; a relay run with a secret only takes
//     lobbies from games that know it.
//
//   NetSession net;
//   net.host(game, 27015, error);           // or net.join(game, "192.168.1.5:27015", error)
//   net.advertise("relay.example.com", "My Game", "Mikel's room", 8, error);   // (optional) -> net.lobbyCode()
//   net.joinLobby(game, "relay.example.com", "K7QX2M", error);                 // (another machine)
//   every frame, after game.frame(): net.update(dt);
#pragma once

#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "game/game.h"
#include "net/crypto.h"
#include "net/relay.h"
#include "net/udp.h"

class NetSession {
public:
    static constexpr uint16_t kDefaultPort = 27015;

    NetSession() = default;
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;
    ~NetSession() { stop(); }

    // Before host() / join(): the game's password (a host lets in only players with the same one; "": none)
    void setPassword(const std::string& password) { password_ = password; }
    // Before advertise(): what the relay asks hosts for, if it was started with a secret (vkrelay --secret)
    void setRelaySecret(const std::string& secret) { relaySecret_ = secret; }

    // port 0: any free one (port() tells); dedicated: no player of its own (a server nobody plays on)
    bool host(Game& game, uint16_t port, std::string& error, bool dedicated = false);
    bool join(Game& game, const std::string& address, std::string& error);   // "host[:port]"
    // A host's game listed on a relay ("host[:port]") as `name` among `game`'s lobbies, up to maxPlayers; its code
    // comes back in a moment (lobbyCode()). Kept there until stop(). Not listed: only joined by its code.
    bool advertise(const std::string& relayAddress, const std::string& game, const std::string& name, int maxPlayers,
                   std::string& error, bool listed = true);
    // Joins the lobby with that code through the relay (directly if the NATs let it, else through the relay)
    bool joinLobby(Game& game, const std::string& relayAddress, const std::string& code, std::string& error);
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
    bool dedicated() const { return dedicated_; }
    const std::string& lobbyCode() const { return lobbyCode_; }   // (a host on a relay, once it answered)
    net::Address publicAddress() const { return publicAddress_; } // ... and where the relay sees it from
    bool throughRelay() const { return !server_ && relayed(serverAddress_); }   // (a client: no direct way)
    bool lobbyFailed() const { return lobbyFailed_; }   // (a client: the relay knows no such lobby)
    bool refused() const { return refused_; }           // (a client: the server turned it away: status() says why)
    bool relayRefused() const { return relayRefused_; } // (a host: the relay wants a secret this game doesn't have)

    // Testing and tuning: every packet this machine sends waits `latency` seconds and is lost with probability
    // `loss` (a fixed random sequence); lag compensation can be turned off
    void simulateNetwork(float latency, float loss);
    // ... and no packet goes directly to another peer (a NAT that can't be punched through): only via the relay
    void simulateStrictNat(bool on) { strictNat_ = on; }
    void setLagCompensation(bool on) { lagCompensation_ = on; }
    uint64_t bytesSent() const { return bytesSent_; }
    float roundTrip() const { return rtt_; }   // a client's measured round trip (seconds)
    uint64_t corrections() const { return corrections_; }   // a client's: how often the server put its player right

private:
    struct Reliable {                // messages to one peer: numbered, resent until it acknowledges them
        uint32_t next = 1;           // the next one's number
        uint32_t sentUpTo = 0;       // ... the last sent at least once
        std::deque<std::pair<uint32_t, std::vector<uint8_t>>> unacked;   // (encoded)
        uint32_t received = 0;       // the last one delivered from the peer (in order)
        bool ackDue = false;
        double resendAt = 0.0;
    };
    struct Input {
        uint32_t seq = 0;
        std::vector<float> values;   // the actions', in order
        float mouse[2] = {0, 0};
        double arrived = 0.0;        // (server: when it came)
    };
    struct Sent {                    // (server: a snapshot as a client was sent it)
        std::map<EntityId, std::vector<uint8_t>> blobs;
        std::string state;
        std::vector<EntityId> gone;
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
        uint64_t acked = 0;          // the last snapshot it got whole
        std::map<uint64_t, Sent> sent;   // the snapshots not yet acknowledged (and the acknowledged one)
        std::set<EntityId> goneKnown;    // destructions it has acknowledged
        float rtt = 0.f, waited = 0.f;   // its round trip; how long its last input waited here
        crypto::Channel channel;
        Reliable messages;
    };
    struct Pending {                 // (server: a key exchange begun, the client's first sealed packet not yet in)
        net::Address address;
        crypto::Key theirs{};
        crypto::KeyPair mine;
        crypto::Channel channel;
        double at = 0.0;
        int failures = 0;
    };
    struct Partial {                 // (client: a snapshot arriving in parts)
        std::vector<std::vector<uint8_t>> parts;
        size_t have = 0;
    };
    struct Got {                     // (client: a whole snapshot, as a baseline for the next ones)
        std::map<EntityId, std::vector<uint8_t>> blobs;
        std::string state;
    };
    struct Move {                    // (client: what its player did with one input)
        uint32_t seq = 0;
        Vec3 delta;
    };

    void send(const net::Address& to, const std::vector<uint8_t>& bytes);
    void sendSealed(const net::Address& to, crypto::Channel& channel, const std::vector<uint8_t>& packet);
    void toServer(const std::vector<uint8_t>& packet) { sendSealed(serverAddress_, channel_, packet); }
    void handshakeReceive(uint8_t type, const uint8_t* data, size_t size, const net::Address& from);
    void sendOffer();
    void queueMessages();
    void flushMessages(Reliable& r, const net::Address& to, crypto::Channel& channel);
    void receiveMessages(Reliable& r, const uint8_t* data, size_t size, uint16_t client);
    void sendRelay(const relay::Message& m);
    void relayReceive(const uint8_t* data, size_t size);
    void receive(const uint8_t* data, size_t size, const net::Address& from);
    void updateRelay();
    static uint64_t addressKey(const net::Address& a) { return (uint64_t)a.ip << 16 | a.port; }
    bool relayed(const net::Address& a) const { return relayed_.count(addressKey(a)) > 0; }
    void flushDelayed();
    void serverReceive(const uint8_t* data, size_t size, const net::Address& from, crypto::Channel* fresh = nullptr);
    void clientReceive(const uint8_t* data, size_t size);
    void sendSnapshots();
    void beforeTick();
    void afterTick();
    void dropClient(size_t index, const char* why);
    void applySnapshot(uint64_t tick, const std::vector<uint8_t>& payload);
    void applyEntity(EntityId id, const std::vector<uint8_t>& blob);
    void reconcile(uint32_t applied, const Transform& server);
    void smooth(float dt);
    void hookGame();
    void recordHistory();
    void rewind(EntityId player);
    void unrewind();

    Game* game_ = nullptr;
    net::UdpSocket socket_;
    bool server_ = false;
    double clock_ = 0.0;   // seconds: the frames' time (not the wall clock), so tests can run faster than real time
    std::string status_;
    uint64_t bytesSent_ = 0;
    // Simulated network
    float latency_ = 0.f, loss_ = 0.f;
    std::mt19937 lossRng_{12345};
    struct Delayed {
        double at;
        net::Address to;
        std::vector<uint8_t> bytes;
    };
    std::deque<Delayed> delayed_;
    // Server
    std::vector<Client> clients_;
    uint16_t nextClient_ = 1;
    float snapshotTimer_ = 0.f;
    uint64_t snapshotTick_ = 0;
    std::set<EntityId> known_;       // entities clients may have (scene ones, replicated ones sent)
    std::map<EntityId, uint64_t> destroyed_;   // ... and gone since, with the snapshot they went in
    std::string scenePath_;
    bool lagCompensation_ = true;
    bool dedicated_ = false;
    std::string password_;
    std::vector<Pending> pending_;
    std::map<uint32_t, std::pair<int, double>> wrongPasswords_;   // an ip's recent wrong passwords, and when the last was
    // Relay (net/relay.h): where it is; the peers only reached through it; peers being punched through to (until)
    net::Address relay_;
    bool useRelay_ = false, strictNat_ = false;
    std::set<uint64_t> relayed_;
    std::vector<std::pair<net::Address, double>> punching_;
    double punchAt_ = 0.0;
    // ... a host's lobby
    std::string lobbyGame_, lobbyName_, lobbyCode_;
    int lobbyMax_ = 0;
    bool lobbyListed_ = true;
    std::string relaySecret_;
    std::array<uint8_t, 16> relayNonce_{};   // (the relay's challenge, answered in each Register)
    bool relayChallenged_ = false, relayRefused_ = false;
    uint32_t lobbyToken_ = 0;
    double registerAt_ = -1e9;
    net::Address publicAddress_;
    // ... a client finding one
    std::string joinCode_;
    bool finding_ = false, lobbyFailed_ = false;
    double connectAt_ = -1e9, peerAt_ = 0.0;
    struct Pose {
        float time;
        std::map<EntityId, Transform> transforms;
    };
    std::deque<Pose> history_;       // the last second of replicated colliders' transforms
    std::map<EntityId, Transform> rewound_;   // (while rewound: where they really are)
    // Client
    net::Address serverAddress_;
    crypto::KeyPair keys_;
    crypto::Channel channel_;
    Reliable messages_;
    bool refused_ = false;
    bool welcomed_ = false;
    uint16_t clientId_ = 0;
    EntityId localPlayer_ = kNoEntity;
    double helloAt_ = -1.0, heardAt_ = 0.0, pingAt_ = 0.0;
    float rtt_ = 0.f;
    uint32_t inputSeq_ = 0;
    std::deque<Input> sentInputs_;   // the last few (each packet carries them all: losses don't matter)
    std::deque<Move> moves_;         // the player's moves the server hasn't confirmed yet
    Vec3 tickStart_;
    uint64_t corrections_ = 0;
    uint64_t lastSnapshot_ = 0;
    std::map<uint64_t, Partial> partials_;
    std::map<uint64_t, Got> got_;    // whole snapshots kept as baselines (the last few)
    std::set<EntityId> shown_;       // entities in the last snapshot applied (to hide ones that left its interest)
    bool marked_ = false;            // the server's entities in this scene marked as its own
    std::string markedPath_;
    struct Target {
        Vec3 position, scale;
        Quat rotation;
    };
    std::map<EntityId, Target> targets_;   // where the server has the entities this client shows
};
