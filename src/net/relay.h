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
//
// The protocol is its own ("VKR1"), over UDP, next to the game's (net/net_session.h). Plain code with no engine
// dependencies: vkrelay is built from this and net/udp only.
#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "net/udp.h"

namespace relay {

constexpr uint16_t kDefaultPort = 27100;
constexpr double kLobbyTimeout = 10.0;   // seconds without hearing from a host: its lobby is gone
constexpr double kPairTimeout = 60.0;    // ... without traffic between two peers it introduced: no more forwarding

struct Lobby {
    std::string code, name;
    int players = 0, maxPlayers = 0;
};

enum class Type : uint8_t {
    Register = 1,   // host -> relay: token, game, name, players, maxPlayers
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
};

struct Message {
    Type type = Type::Punch;
    uint32_t token = 0;
    std::string game, name, code;
    int players = 0, maxPlayers = 0;
    net::Address address;
    bool host = false;
    std::vector<Lobby> lobbies;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode(const Message& m);
bool decode(const uint8_t* data, size_t size, Message& out);
bool isRelayPacket(const uint8_t* data, size_t size);   // (starts with "VKR1")

// A lobby code: six letters and digits, none that look alike (no 0 / O, 1 / I)
std::string makeCode(std::mt19937& rng);

class Server {
public:
    bool open(uint16_t port, std::string& error);
    void close() { socket_.close(); }
    bool isOpen() const { return socket_.isOpen(); }
    uint16_t port() const { return socket_.port(); }
    // Handles what has arrived; `now` in seconds (any clock that goes forward: timeouts)
    void update(double now);

    size_t lobbyCount() const { return lobbies_.size(); }
    uint64_t forwarded() const { return forwarded_; }   // packets relayed between peers
    bool verbose = false;   // log lobbies coming and going, joins

private:
    struct Hosted {
        net::Address host;
        uint32_t token = 0;
        std::string game, name;
        int players = 0, maxPlayers = 0;
        double heard = 0.0;
    };
    void handle(const Message& m, const net::Address& from, double now);
    void reply(const net::Address& to, const Message& m);
    static uint64_t key(const net::Address& a) { return (uint64_t)a.ip << 16 | a.port; }
    static std::pair<uint64_t, uint64_t> pair(const net::Address& a, const net::Address& b) {
        return key(a) < key(b) ? std::make_pair(key(a), key(b)) : std::make_pair(key(b), key(a));
    }

    net::UdpSocket socket_;
    std::map<std::string, Hosted> lobbies_;   // by code
    std::map<std::pair<uint64_t, uint64_t>, double> pairs_;   // peers introduced -> last traffic
    std::mt19937 rng_{std::random_device{}()};
    uint64_t forwarded_ = 0;
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
