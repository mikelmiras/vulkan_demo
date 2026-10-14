#include "net/relay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

// Packets: "VKR2", the type (u8), then its fields (little-endian; strings as a u16 length and the bytes; addresses
// as ip (u32) and port (u16)):
//   Register    token (u32), game, name, players (u8), maxPlayers (u8), flags (u8: 1 listed, 2 password), nonce (16
//               bytes), proof (32; zeros without a secret)
//   Registered  token, code, address
//   Unregister  token
//   List        game
//   Lobbies     count (u8), each: code, name, players (u8), maxPlayers (u8), password (u8)
//   Connect     code
//   Peer        code, address, host (u8)
//   NoLobby     code
//   Forward     address, then the payload to the end
//   Forwarded   address, then the payload to the end
//   Punch       (nothing)
//   Challenge   nonce (16 bytes)
namespace relay {

namespace {

constexpr uint8_t kMagic[4] = {'V', 'K', 'R', '2'};
constexpr size_t kMaxNonces = 65536;       // challenges outstanding (more: new hosts wait)
constexpr size_t kMaxLobbiesListed = 40;   // (a list fits in one packet)

struct Writer {
    std::vector<uint8_t> bytes;
    template <class T> void put(T v) {
        uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        bytes.insert(bytes.end(), b, b + sizeof(T));
    }
    void str(const std::string& s) {
        size_t n = std::min<size_t>(s.size(), 255);   // (names and codes: short)
        put((uint16_t)n);
        bytes.insert(bytes.end(), s.begin(), s.begin() + (long)n);
    }
    void address(const net::Address& a) {
        put(a.ip);
        put(a.port);
    }
};

struct Reader {
    const uint8_t* data;
    size_t size, pos = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (pos + sizeof(T) > size) { ok = false; return v; }
        std::memcpy(&v, data + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    std::string str() {
        uint16_t n = get<uint16_t>();
        if (!ok || pos + n > size) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return s;
    }
    net::Address address() {
        net::Address a;
        a.ip = get<uint32_t>();
        a.port = get<uint16_t>();
        return a;
    }
};

}  // namespace

bool isRelayPacket(const uint8_t* data, size_t size) { return size >= 5 && std::memcmp(data, kMagic, 4) == 0; }

std::vector<uint8_t> encode(const Message& m) {
    Writer w;
    w.bytes.assign(kMagic, kMagic + 4);
    w.put((uint8_t)m.type);
    switch (m.type) {
    case Type::Register:
        w.put(m.token);
        w.str(m.game);
        w.str(m.name);
        w.put((uint8_t)std::clamp(m.players, 0, 255));
        w.put((uint8_t)std::clamp(m.maxPlayers, 0, 255));
        w.put((uint8_t)((m.listed ? 1 : 0) | (m.password ? 2 : 0)));
        w.bytes.insert(w.bytes.end(), m.nonce.begin(), m.nonce.end());
        w.bytes.insert(w.bytes.end(), m.proof.begin(), m.proof.end());
        break;
    case Type::Registered:
        w.put(m.token);
        w.str(m.code);
        w.address(m.address);
        break;
    case Type::Unregister: w.put(m.token); break;
    case Type::List: w.str(m.game); break;
    case Type::Lobbies:
        w.put((uint8_t)std::min(m.lobbies.size(), kMaxLobbiesListed));
        for (size_t i = 0; i < m.lobbies.size() && i < kMaxLobbiesListed; ++i) {
            w.str(m.lobbies[i].code);
            w.str(m.lobbies[i].name);
            w.put((uint8_t)std::clamp(m.lobbies[i].players, 0, 255));
            w.put((uint8_t)std::clamp(m.lobbies[i].maxPlayers, 0, 255));
            w.put((uint8_t)(m.lobbies[i].password ? 1 : 0));
        }
        break;
    case Type::Connect:
    case Type::NoLobby: w.str(m.code); break;
    case Type::Peer:
        w.str(m.code);
        w.address(m.address);
        w.put((uint8_t)(m.host ? 1 : 0));
        break;
    case Type::Forward:
    case Type::Forwarded:
        w.address(m.address);
        w.bytes.insert(w.bytes.end(), m.payload.begin(), m.payload.end());
        break;
    case Type::Punch: break;
    case Type::Challenge: w.bytes.insert(w.bytes.end(), m.nonce.begin(), m.nonce.end()); break;
    }
    return w.bytes;
}

bool decode(const uint8_t* data, size_t size, Message& out) {
    if (!isRelayPacket(data, size)) return false;
    Reader r{data, size, 5};
    out = {};
    out.type = (Type)data[4];
    switch (out.type) {
    case Type::Register:
        out.token = r.get<uint32_t>();
        out.game = r.str();
        out.name = r.str();
        out.players = r.get<uint8_t>();
        out.maxPlayers = r.get<uint8_t>();
        {
            uint8_t flags = r.get<uint8_t>();
            out.listed = (flags & 1) != 0;
            out.password = (flags & 2) != 0;
        }
        for (uint8_t& b : out.nonce) b = r.get<uint8_t>();
        for (uint8_t& b : out.proof) b = r.get<uint8_t>();
        break;
    case Type::Registered:
        out.token = r.get<uint32_t>();
        out.code = r.str();
        out.address = r.address();
        break;
    case Type::Unregister: out.token = r.get<uint32_t>(); break;
    case Type::List: out.game = r.str(); break;
    case Type::Lobbies: {
        uint8_t n = r.get<uint8_t>();
        for (uint8_t i = 0; i < n && r.ok; ++i) {
            Lobby l;
            l.code = r.str();
            l.name = r.str();
            l.players = r.get<uint8_t>();
            l.maxPlayers = r.get<uint8_t>();
            l.password = r.get<uint8_t>() != 0;
            if (r.ok) out.lobbies.push_back(l);
        }
        break;
    }
    case Type::Connect:
    case Type::NoLobby: out.code = r.str(); break;
    case Type::Peer:
        out.code = r.str();
        out.address = r.address();
        out.host = r.get<uint8_t>() != 0;
        break;
    case Type::Forward:
    case Type::Forwarded:
        out.address = r.address();
        if (r.ok) out.payload.assign(data + r.pos, data + size);
        break;
    case Type::Punch: break;
    case Type::Challenge:
        for (uint8_t& b : out.nonce) b = r.get<uint8_t>();
        break;
    default: return false;
    }
    return r.ok;
}

crypto::Key registerProof(const std::string& secret, const Message& m) {
    crypto::Key key = crypto::hash("vkrelay secret\n" + secret);
    crypto::Blake2s b(32, key.data(), key.size());
    b.update(m.nonce.data(), m.nonce.size()).update(&m.token, sizeof(m.token));
    b.update(m.game).update("\n").update(m.name);
    crypto::Key proof;
    b.final(proof.data());
    return proof;
}

std::string makeCode(std::mt19937& rng) {
    static const char kLetters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string code;
    for (int i = 0; i < 6; ++i) code += kLetters[std::uniform_int_distribution<int>(0, 31)(rng)];
    return code;
}

// ---------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------
bool Server::open(uint16_t port, std::string& error) { return socket_.open(port, error); }

void Server::reply(const net::Address& to, const Message& m) {
    std::vector<uint8_t> bytes = encode(m);
    socket_.send(to, bytes.data(), bytes.size());
}

void Server::update(double now) {
    if (!socket_.isOpen()) return;
    uint8_t buf[2048];
    net::Address from;
    for (size_t n; (n = socket_.receive(from, buf, sizeof(buf))) > 0;) {
        Message m;
        if (!allow(from, n, now)) continue;
        if (decode(buf, n, m)) handle(m, from, now);
    }
    for (auto it = lobbies_.begin(); it != lobbies_.end();) {
        if (now - it->second.heard > kLobbyTimeout) {
            if (verbose) std::printf("lobby %s (%s) timed out\n", it->first.c_str(), it->second.name.c_str());
            it = lobbies_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pairs_.begin(); it != pairs_.end();)
        it = now - it->second > kPairTimeout ? pairs_.erase(it) : std::next(it);
    for (auto it = nonces_.begin(); it != nonces_.end();)
        it = now - it->second.at > kPairTimeout ? nonces_.erase(it) : std::next(it);
    for (auto it = buckets_.begin(); it != buckets_.end();)   // (full again after a quiet while: forgotten)
        it = now - it->second.at > kPairTimeout ? buckets_.erase(it) : std::next(it);
}

// Token buckets per ip: each packet costs one and its size, refilled at the limits' rates up to their bursts
bool Server::allow(const net::Address& from, size_t bytes, double now) {
    auto it = buckets_.find(from.ip);
    if (it == buckets_.end()) it = buckets_.emplace(from.ip, Bucket{limits.packetBurst, limits.byteBurst, now}).first;
    Bucket& b = it->second;
    double dt = std::max(now - b.at, 0.0);
    b.packets = std::min(limits.packetBurst, b.packets + dt * limits.packetsPerSecond);
    b.bytes = std::min(limits.byteBurst, b.bytes + dt * limits.bytesPerSecond);
    b.at = now;
    if (b.packets < 1.0 || b.bytes < (double)bytes) {
        ++dropped_;
        return false;
    }
    b.packets -= 1.0;
    b.bytes -= (double)bytes;
    return true;
}

void Server::handle(const Message& m, const net::Address& from, double now) {
    switch (m.type) {
    case Type::Register: {   // (again: the same lobby, its player count brought up to date)
        if (!secret_.empty()) {   // (only hosts that know the secret: a challenge, answered with the proof)
            auto n = nonces_.find(key(from));
            if (n == nonces_.end() || !crypto::equal(n->second.nonce.data(), m.nonce.data(), 16) ||
                !crypto::equal(registerProof(secret_, m).data(), m.proof.data(), 32)) {
                if (n == nonces_.end()) {
                    if (nonces_.size() >= kMaxNonces) break;
                    Nonce fresh;
                    crypto::randomBytes(fresh.nonce.data(), fresh.nonce.size());
                    fresh.at = now;
                    n = nonces_.emplace(key(from), fresh).first;
                }
                Message c;
                c.type = Type::Challenge;
                c.nonce = n->second.nonce;
                reply(from, c);
                break;
            }
            n->second.at = now;
        }
        auto it = lobbies_.begin();
        for (; it != lobbies_.end(); ++it)
            if (it->second.host == from && it->second.token == m.token) break;
        if (it == lobbies_.end()) {
            int hosted = 0;
            for (const auto& [code, h] : lobbies_) hosted += h.host.ip == from.ip;
            if (hosted >= limits.lobbies) break;
            std::string code;
            do code = makeCode(rng_);
            while (lobbies_.count(code));
            it = lobbies_.emplace(code, Hosted{from, m.token, m.game, m.name, 0, 0, now}).first;
            if (verbose) std::printf("lobby %s: %s (%s) hosted by %s\n", code.c_str(), m.name.c_str(), m.game.c_str(), from.str().c_str());
        }
        Hosted& h = it->second;
        h.name = m.name;
        h.players = m.players;
        h.maxPlayers = m.maxPlayers;
        h.listed = m.listed;
        h.password = m.password;
        h.heard = now;
        Message r;
        r.type = Type::Registered;
        r.token = m.token;
        r.code = it->first;
        r.address = from;
        reply(from, r);
        break;
    }
    case Type::Unregister:
        for (auto it = lobbies_.begin(); it != lobbies_.end(); ++it)
            if (it->second.host == from && it->second.token == m.token) {
                if (verbose) std::printf("lobby %s closed\n", it->first.c_str());
                lobbies_.erase(it);
                break;
            }
        break;
    case Type::List: {
        Message r;
        r.type = Type::Lobbies;
        for (const auto& [code, h] : lobbies_)
            if (h.game == m.game && h.listed) r.lobbies.push_back({code, h.name, h.players, h.maxPlayers, h.password});
        reply(from, r);
        break;
    }
    case Type::Connect: {
        auto it = lobbies_.find(m.code);
        if (it == lobbies_.end()) {
            Message r;
            r.type = Type::NoLobby;
            r.code = m.code;
            reply(from, r);
            break;
        }
        // Each told where the other is: they punch through to each other (or forward through here)
        pairs_[pair(from, it->second.host)] = now;
        Message toHost, toPlayer;
        toHost.type = toPlayer.type = Type::Peer;
        toHost.code = toPlayer.code = m.code;
        toHost.address = from;
        toHost.host = true;
        toPlayer.address = it->second.host;
        reply(it->second.host, toHost);
        reply(from, toPlayer);
        if (verbose) std::printf("%s joins lobby %s\n", from.str().c_str(), m.code.c_str());
        break;
    }
    case Type::Forward: {
        auto it = pairs_.find(pair(from, m.address));
        if (it == pairs_.end()) break;   // (only between peers introduced here)
        it->second = now;
        Message r;
        r.type = Type::Forwarded;
        r.address = from;
        r.payload = m.payload;
        reply(m.address, r);
        ++forwarded_;
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Listing lobbies
// ---------------------------------------------------------------------------------------------
bool LobbyQuery::start(const std::string& relayAddress, const std::string& game, std::string& error) {
    socket_.close();
    lobbies_.clear();
    error_.clear();
    waited_ = 0.f;
    done_ = false;
    if (!net::resolve(relayAddress, kDefaultPort, relay_, error)) return false;
    if (!socket_.open(0, error)) return false;
    game_ = game;
    resend_ = 1e9f;   // (asks at once)
    return true;
}

void LobbyQuery::update(float dt) {
    if (!socket_.isOpen()) return;
    waited_ += dt;
    resend_ += dt;
    if (resend_ >= 0.5f) {   // (asked again until it answers: a packet may be lost)
        Message m;
        m.type = Type::List;
        m.game = game_;
        std::vector<uint8_t> bytes = encode(m);
        socket_.send(relay_, bytes.data(), bytes.size());
        resend_ = 0.f;
    }
    uint8_t buf[2048];
    net::Address from;
    for (size_t n; (n = socket_.receive(from, buf, sizeof(buf))) > 0;) {
        Message m;
        if (from == relay_ && decode(buf, n, m) && m.type == Type::Lobbies) {
            lobbies_ = m.lobbies;
            done_ = true;
            socket_.close();
            return;
        }
    }
    if (waited_ > 3.f) {
        error_ = "no answer from the relay " + relay_.str();
        done_ = true;
        socket_.close();
    }
}

}  // namespace relay
