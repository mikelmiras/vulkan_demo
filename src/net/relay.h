// The relay: a small server on the internet that multiplayer games meet through (vkrelay runs one).
//
//   - Lobbies: a host registers its game with the relay and gets a short code ("K7QX2M") to give friends; players
//     list a game's lobbies or join one by its code. A host keeps its lobby by registering again every few seconds
//     (with the players it has); one not heard from for 10 s is gone.
//   - NAT traversal: on a join, the relay tells each side where the other is as seen from the internet (its public
//     address), and both send each other a few Punch packets: most home routers then let the other's packets in
//     (UDP hole punching), and the game goes straight between them.
//   - Relaying: when that doesn't work (strict NATs, firewalls), packets go through the relay instead (Forward:
//     "send this to that peer"; it arrives as Forwarded "from that peer"). The relay only forwards between peers it
//     introduced.
//   - Private lobbies aren't listed (only joined by their code); a lobby can say it has a password (the game checks
//     it: net/net_session.h), shown in lists.
//   - A relay started with a secret (vkrelay --secret) only takes lobbies from hosts that know it: it answers a
//     Register with a Challenge (a random nonce), and takes the Register again with a proof (BLAKE2s keyed by the
//     secret over the nonce and the lobby), so the secret itself is never sent.
//   - Rate limits per ip (packets and bytes a second, lobbies at once): a flood is dropped, not relayed.
//
// The protocol is its own ("VKR2"), over UDP, next to the game's (net/net_session.h). Plain code with no engine
// dependencies: vkrelay is built from this and net/udp only.
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "net/crypto.h"
#include "net/udp.h"

namespace relay {

constexpr uint16_t kDefaultPort = 27100;
constexpr double kLobbyTimeout = 10.0;   // seconds without hearing from a host: its lobby is gone
constexpr double kPairTimeout = 60.0;    // ... without traffic between two peers it introduced: no more forwarding

struct Lobby {
    std::string code, name;
    int players = 0, maxPlayers = 0;
    bool password = false;
};

enum class Type : uint8_t {
    Register = 1,   // host -> relay: token, game, name, players, maxPlayers, flags (listed, password), nonce, proof
    Registered,     // relay -> host: token, code, address (the host's public one)
    Unregister,     // host -> relay: token
    List,           // player -> relay: game
    Lobbies,        // relay -> player: lobbies (the first 40)
    Connect,        // player -> relay: code
    Peer,           // relay -> both: code, address (the other's public one), host (true: sent to the host)
    NoLobby,        // relay -> player: code
    Forward,        // peer -> relay: address (where to), payload
    Forwarded,      // relay -> peer: address (where from), payload
    Punch,          // peer -> peer: nothing (it opens the way through the sender's NAT)
    Challenge,      // relay -> host: nonce (a relay with a secret: Register again with the proof)
};

struct Message {
    Type type = Type::Punch;
    uint32_t token = 0;
    std::string game, name, code;
    int players = 0, maxPlayers = 0;
    net::Address address;
    bool host = false;
    bool listed = true, password = false;
    std::array<uint8_t, 16> nonce{};
    crypto::Key proof{};
    std::vector<Lobby> lobbies;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode(const Message& m);
bool decode(const uint8_t* data, size_t size, Message& out);
bool isRelayPacket(const uint8_t* data, size_t size);   // (starts with "VKR1")

// What proves a Register knows the relay's secret: BLAKE2s keyed by it, over the challenge's nonce and the lobby
crypto::Key registerProof(const std::string& secret, const Message& m);

// A lobby code: six letters and digits, none that look alike (no 0 / O, 1 / I)
std::string makeCode(std::mt19937& rng);

class Server {
public:
    struct Limits {                         // per ip
        double packetsPerSecond = 500.0, packetBurst = 1000.0;
        double bytesPerSecond = 640.0 * 1024.0, byteBurst = 1280.0 * 1024.0;
        int lobbies = 8;                    // hosted at once
    };
    Limits limits;
    void setSecret(const std::string& secret) { secret_ = secret; }   // "": anyone may host
    bool open(uint16_t port, std::string& error);
    void close() { socket_.close(); }
    bool isOpen() const { return socket_.isOpen(); }
    uint16_t port() const { return socket_.port(); }
    // Handles what has arrived; `now` in seconds (any clock that goes forward: timeouts)
    void update(double now);

    size_t lobbyCount() const { return lobbies_.size(); }
    uint64_t forwarded() const { return forwarded_; }   // packets relayed between peers
    uint64_t dropped() const { return dropped_; }       // packets over an ip's rate limit
    bool verbose = false;   // log lobbies coming and going, joins

private:
    struct Hosted {
        net::Address host;
        uint32_t token = 0;
        std::string game, name;
        int players = 0, maxPlayers = 0;
        double heard = 0.0;
        bool listed = true, password = false;
    };
    void handle(const Message& m, const net::Address& from, double now);
    void reply(const net::Address& to, const Message& m);
    bool allow(const net::Address& from, size_t bytes, double now);   // (the rate limits)
    static uint64_t key(const net::Address& a) { return (uint64_t)a.ip << 16 | a.port; }
    static std::pair<uint64_t, uint64_t> pair(const net::Address& a, const net::Address& b) {
        return key(a) < key(b) ? std::make_pair(key(a), key(b)) : std::make_pair(key(b), key(a));
    }

    net::UdpSocket socket_;
    std::map<std::string, Hosted> lobbies_;   // by code
    std::map<std::pair<uint64_t, uint64_t>, double> pairs_;   // peers introduced -> last traffic
    std::mt19937 rng_{std::random_device{}()};
    uint64_t forwarded_ = 0, dropped_ = 0;
    std::string secret_;
    struct Nonce {
        std::array<uint8_t, 16> nonce;
        double at;
    };
    std::map<uint64_t, Nonce> nonces_;        // challenges given out, by address (the hosts' answers checked against)
    struct Bucket {
        double packets, bytes, at;
    };
    std::map<uint32_t, Bucket> buckets_;      // by ip
};

// Asks a relay for a game's lobbies (in the background: update() each frame until done())
class LobbyQuery {
public:
    bool start(const std::string& relayAddress, const std::string& game, std::string& error);
    void update(float dt);
    bool busy() const { return socket_.isOpen(); }
    bool done() const { return done_; }
    const std::vector<Lobby>& lobbies() const { return lobbies_; }
    const std::string& error() const { return error_; }

private:
    net::UdpSocket socket_;
    net::Address relay_;
    std::string game_, error_;
    std::vector<Lobby> lobbies_;
    float waited_ = 0.f, resend_ = 0.f;
    bool done_ = false;
};

}  // namespace relay
