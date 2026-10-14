#include "net/net_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "scene/serialization.h"

// Packets: "VKN3", a type, then the type's fields (little-endian). In the clear, the key exchange:
//   Offer    (client -> server)  its X25519 public key (32 bytes), 64 zeros (the answer is no bigger than this)
//   Accept   (server -> client)  its public key, the client's (what it answers)
//   Refused  (server -> client)  why (a string): a wrong password, too many of them
//   Sealed   (both)              counter (u64), then a whole packet below ("VKN3", type, fields) sealed with
//                                ChaCha20-Poly1305 (crypto::Channel), the tag last
// Sealed (nothing else is taken once the keys are agreed):
//   Hello    (client -> server)  name
//   Welcome  (server -> client)  client id (u16), its player (u32), the server's scene (string)
//   Input    (client -> server)  client id, the last snapshot it got whole (u64), its round trip (f32), count (u8),
//                                then each: seq (u32), values (u8 count, u8 each), mouse (2 f32)
//   Snapshot (server -> client)  tick (u64), part (u8), parts (u8), bytes (u16 count, then them): the parts put
//                                together are the payload below
//   Bye      (both)              client id
//   Ping     (client -> server)  client id, its clock (f64); Pong (server -> client): the same clock back
//   Messages (both)              the last message received in order (u32), count (u8), each: number (u32), to (u32),
//                                from (u32), value (f32), name, text: GameContext::sendToServer / sendToClients
// A snapshot's payload: its baseline (u64: the snapshot it's a delta from, 0: none), the client's last input applied
// (u32), state (u8 flag: 1 = the game state's JSON follows, 0 = as in the baseline), entities (u16 count; each: id
// (u32), u8: 1 = its blob follows (u16 size, bytes), 0 = as in the baseline), destroyed ids (u16 count, u32 each).
// An entity's blob: parent (u32), flags (u8: 1 visible, 2 prefab follows, 4 fields follow), [prefab name, entity
// count (u16), their ids (u32)], position, rotation, scale (9 f32), [behaviour count (u8), each: type, fields JSON]
namespace {

constexpr uint8_t kMagic[4] = {'V', 'K', 'N', '3'};
enum : uint8_t { kHello = 1, kWelcome, kInput, kSnapshot, kBye, kPing, kPong, kMessages,
                 kOffer = 32, kAccept, kRefused, kSealed };
constexpr size_t kMaxPending = 64;     // key exchanges under way at once (the oldest goes)
constexpr int kMaxWrongPasswords = 5;  // ... from one ip in a minute: it's turned away for that long
constexpr size_t kMaxUnacked = 512;    // messages waiting to be acknowledged (more are dropped, with a warning)
constexpr size_t kMaxPacket = 1200;    // (fits any network's packets without splitting)
constexpr EntityId kClientIds = 1u << 23;   // a client's own entities: ids from here (the server's are lower)
constexpr size_t kPartBytes = 1100;    // payload per snapshot packet
constexpr double kTimeout = 10.0;
constexpr float kSmoothingDelay = 0.1f;   // how far behind the server a client shows others (snapshots, smoothing)

class Writer {
public:
    std::vector<uint8_t> bytes;
    explicit Writer(uint8_t type) { bytes.assign(kMagic, kMagic + 4); bytes.push_back(type); }
    Writer() = default;
    template <class T> void put(T v) {
        uint8_t b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        bytes.insert(bytes.end(), b, b + sizeof(T));
    }
    void str(const std::string& s) {
        put((uint16_t)std::min<size_t>(s.size(), 65535));
        bytes.insert(bytes.end(), s.begin(), s.begin() + (long)std::min<size_t>(s.size(), 65535));
    }
    void append(const std::vector<uint8_t>& b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
};

}  // namespace

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : data_(d), size_(n) {}
    bool ok() const { return ok_; }
    template <class T> T get() {
        T v{};
        if (pos_ + sizeof(T) > size_) { ok_ = false; return v; }
        std::memcpy(&v, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::string str() {
        uint16_t n = get<uint16_t>();
        if (!ok_ || pos_ + n > size_) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

private:
    const uint8_t* data_;
    size_t size_, pos_ = 0;
    bool ok_ = true;
};

namespace {

// json::write without the layout (smaller packets)
std::string compact(const json::Value& v) {
    std::string pretty = json::write(v), out;
    out.reserve(pretty.size());
    bool inString = false, escaped = false;
    for (char c : pretty) {
        if (inString) {
            out += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else if (c == '"') {
            inString = true;
            out += c;
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            out += c;
        }
    }
    return out;
}

bool readHeader(Reader& r, uint8_t& type) {
    for (uint8_t m : kMagic)
        if (r.get<uint8_t>() != m) return false;
    type = r.get<uint8_t>();
    return r.ok();
}

// The prefab instance `root` is the root of, with its entities in the prefab's order (to make it again elsewhere)
const Prefab* instanceOf(const Scene& scene, const Entity& root, std::vector<EntityId>& ids) {
    if (!root.prefab) return nullptr;
    const Prefab* p = nullptr;
    for (const Prefab& pf : scene.prefabs)
        if (pf.id == root.prefab->prefab) p = &pf;
    if (!p || p->entities.empty() || root.prefab->source != p->entities[0].id) return nullptr;
    ids.clear();
    for (const Entity& pe : p->entities) {
        EntityId found = kNoEntity;
        for (const Entity& e : scene.entities)
            if (e.prefab && e.prefab->prefab == p->id && e.prefab->source == pe.id &&
                (e.id == root.id || scene.isAncestor(root.id, e.id)))
                found = e.id;
        ids.push_back(found);
    }
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Starting and stopping
// ---------------------------------------------------------------------------------------------
void NetSession::hookGame() {
    game_->setTickHooks([this] { beforeTick(); }, [this] { afterTick(); });
    if (server_) game_->setRewindHooks([this](EntityId player) { rewind(player); }, [this] { unrewind(); });
}

bool NetSession::host(Game& game, uint16_t port, std::string& error, bool dedicated) {
    stop();
    dedicated_ = dedicated;
    if (!game.running()) { error = "start the game first"; return false; }
    if (!socket_.open(port, error)) return false;
    game_ = &game;
    server_ = true;
    clock_ = 0.0;
    pending_.clear();
    wrongPasswords_.clear();
    game.setNetRole(dedicated ? NetRole::DedicatedServer : NetRole::Server);
    hookGame();
    // What clients may have of this scene (their own copy of it): its entities, gone once destroyed here
    Scene& scene = *game.scene();
    known_.clear();
    destroyed_.clear();
    for (const Entity& e : scene.entities) known_.insert(e.id);
    scenePath_ = game.scenePath();
    // The host plays too (unless it's a dedicated server): its own player from the prefab, if the scene names one
    if (!dedicated && !scene.network.playerPrefab.empty()) {
        localPlayer_ = game.context().spawn(scene.network.playerPrefab, scene.network.spawnPoint);
        game.setLocalPlayer(localPlayer_);
    }
    status_ = std::string(dedicated ? "Serving" : "Hosting") + " on port " + std::to_string(socket_.port());
    logLine("multiplayer: %s", status_.c_str());
    return true;
}

bool NetSession::join(Game& game, const std::string& address, std::string& error) {
    stop();
    if (!game.running()) { error = "start the game first"; return false; }
    if (!net::resolve(address, kDefaultPort, serverAddress_, error)) return false;
    if (!socket_.open(0, error)) return false;
    game_ = &game;
    server_ = false;
    clock_ = heardAt_ = 0.0;
    helloAt_ = -1.0;
    marked_ = false;
    keys_ = crypto::KeyPair::generate();   // (new ones every session)
    channel_.reset();
    messages_ = Reliable{};
    refused_ = false;
    game.setNetRole(NetRole::Client);
    game.scene()->setNextId(kClientIds);   // (this machine's own entities stay clear of the server's ids)
    hookGame();
    status_ = "Joining " + serverAddress_.str() + "...";
    return true;
}

bool NetSession::advertise(const std::string& relayAddress, const std::string& game, const std::string& name,
                           int maxPlayers, std::string& error, bool listed) {
    if (!server_ || !socket_.isOpen()) { error = "host a game first"; return false; }
    if (!net::resolve(relayAddress, relay::kDefaultPort, relay_, error)) return false;
    useRelay_ = true;
    lobbyGame_ = game;
    lobbyName_ = name;
    lobbyMax_ = std::clamp(maxPlayers, 1, 255);
    lobbyListed_ = listed;
    relayChallenged_ = relayRefused_ = false;
    lobbyToken_ = std::random_device{}() | 1u;
    lobbyCode_.clear();
    registerAt_ = -1e9;   // (registers on the next update)
    return true;
}

bool NetSession::joinLobby(Game& game, const std::string& relayAddress, const std::string& code, std::string& error) {
    net::Address relayAt;
    if (!net::resolve(relayAddress, relay::kDefaultPort, relayAt, error)) return false;
    if (!join(game, relayAddress, error)) return false;   // (the socket and the client's state; the server's found next)
    relay_ = relayAt;
    useRelay_ = true;
    serverAddress_ = {};
    joinCode_ = code;
    for (char& c : joinCode_) c = (char)std::toupper((unsigned char)c);
    finding_ = true;
    lobbyFailed_ = false;
    connectAt_ = -1e9;
    status_ = "Finding lobby " + joinCode_ + "...";
    return true;
}

void NetSession::stop() {
    if (!socket_.isOpen()) return;
    if (useRelay_ && server_ && !lobbyName_.empty()) {   // (the lobby goes at once, not after the relay's timeout)
        relay::Message bye;
        bye.type = relay::Type::Unregister;
        bye.token = lobbyToken_;
        sendRelay(bye);
    }
    Writer bye(kBye);
    bye.put(clientId_);
    if (server_) {
        for (Client& c : clients_) sendSealed(c.address, c.channel, bye.bytes);
        while (!clients_.empty()) dropClient(clients_.size() - 1, "the server stopped");
    } else if (serverAddress_.port && channel_.ready()) {
        toServer(bye.bytes);
    }
    flushDelayed();
    if (game_) {
        game_->setTickHooks(nullptr, nullptr);
        game_->setRewindHooks(nullptr, nullptr);
        game_->setNetRole(NetRole::Offline);
    }
    pending_.clear();
    channel_.reset();
    messages_ = Reliable{};
    socket_.close();
    welcomed_ = false;
    targets_.clear();
    partials_.clear();
    got_.clear();
    moves_.clear();
    delayed_.clear();
    history_.clear();
    useRelay_ = finding_ = dedicated_ = false;
    relayed_.clear();
    punching_.clear();
    lobbyName_.clear();
    lobbyCode_.clear();
    joinCode_.clear();
    status_ = "Offline";
}

void NetSession::simulateNetwork(float latency, float loss) {
    latency_ = std::max(latency, 0.f);
    loss_ = std::clamp(loss, 0.f, 1.f);
}

// Every packet goes through here: counted, wrapped for the relay when a peer is only reached through it, and (when
// simulating a network) delayed or lost
void NetSession::send(const net::Address& to, const std::vector<uint8_t>& bytes) {
    const net::Address* dest = &to;
    std::vector<uint8_t> wrapped;
    if (useRelay_ && to != relay_ && relayed(to)) {
        relay::Message m;
        m.type = relay::Type::Forward;
        m.address = to;
        m.payload = bytes;
        wrapped = relay::encode(m);
        dest = &relay_;
    } else if (strictNat_ && to != relay_) {
        return;   // (simulated: nothing goes straight through)
    }
    const std::vector<uint8_t>& out = wrapped.empty() ? bytes : wrapped;
    bytesSent_ += out.size();
    if (loss_ > 0.f && std::uniform_real_distribution<float>(0.f, 1.f)(lossRng_) < loss_) return;
    if (latency_ > 0.f) delayed_.push_back({clock_ + latency_, *dest, out});
    else socket_.send(*dest, out.data(), out.size());
}

// A packet sealed for a peer (nothing is sent before the keys are agreed)
void NetSession::sendSealed(const net::Address& to, crypto::Channel& channel, const std::vector<uint8_t>& packet) {
    if (!channel.ready()) return;
    const uint8_t prefix[5] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3], kSealed};
    send(to, channel.seal(prefix, sizeof(prefix), packet.data(), packet.size()));
}

void NetSession::sendOffer() {
    Writer w(kOffer);
    w.bytes.insert(w.bytes.end(), keys_.publicKey.begin(), keys_.publicKey.end());
    w.bytes.resize(w.bytes.size() + 64, 0);
    send(serverAddress_, w.bytes);
}

void NetSession::sendRelay(const relay::Message& m) {
    if (useRelay_) send(relay_, relay::encode(m));
}

// A packet from the relay: a lobby's code, a peer to punch through to (or the server found), a peer's packet
// forwarded (that peer is reached through the relay from now on)
void NetSession::relayReceive(const uint8_t* data, size_t size) {
    relay::Message m;
    if (!relay::decode(data, size, m)) return;
    switch (m.type) {
    case relay::Type::Registered:
        if (!server_ || m.token != lobbyToken_) break;
        if (lobbyCode_ != m.code) logLine("multiplayer: lobby %s on the relay %s (seen from %s)", m.code.c_str(), relay_.str().c_str(),
                                          m.address.str().c_str());
        lobbyCode_ = m.code;
        publicAddress_ = m.address;
        relayRefused_ = false;
        break;
    case relay::Type::Challenge:   // (a relay with a secret: Register again, with the proof)
        if (!server_ || lobbyName_.empty()) break;
        if (relaySecret_.empty() || (relayChallenged_ && m.nonce == relayNonce_)) {   // (no secret, or it didn't take ours)
            if (!relayRefused_) logLine("multiplayer: the relay %s %s", relay_.str().c_str(),
                                        relaySecret_.empty() ? "wants a secret (vkgame --relay-secret)" : "refused this game's secret");
            relayRefused_ = true;
            if (relaySecret_.empty()) break;
        }
        relayNonce_ = m.nonce;
        relayChallenged_ = true;
        registerAt_ = -1e9;
        break;
    case relay::Type::Peer:
        if (server_ && m.host) {   // someone joining: punch through to them (their Hello comes either way)
            punching_.push_back({m.address, clock_ + 3.0});
            punchAt_ = -1e9;
        } else if (!server_ && !m.host && finding_ && m.code == joinCode_) {   // the server: Hello, straight to it
            finding_ = false;
            serverAddress_ = m.address;
            peerAt_ = clock_;
            helloAt_ = -1.0;
            heardAt_ = clock_;
            punching_.push_back({m.address, clock_ + 3.0});
            punchAt_ = -1e9;
            status_ = "Joining lobby " + joinCode_ + " (" + serverAddress_.str() + ")...";
        }
        break;
    case relay::Type::NoLobby:
        if (!server_ && finding_ && m.code == joinCode_) {
            finding_ = false;
            lobbyFailed_ = true;
            status_ = "No lobby " + joinCode_;
            logLine("multiplayer: %s", status_.c_str());
        }
        break;
    case relay::Type::Forwarded:
        if (!m.payload.empty() && !relay::isRelayPacket(m.payload.data(), m.payload.size())) {
            if (!relayed(m.address)) logLine("multiplayer: reaching %s through the relay", m.address.str().c_str());
            relayed_.insert(addressKey(m.address));
            receive(m.payload.data(), m.payload.size(), m.address);
        }
        break;
    default: break;
    }
}

// Every packet from a peer: the key exchange in the clear, everything else opened with that peer's keys (or dropped)
void NetSession::receive(const uint8_t* data, size_t size, const net::Address& from) {
    Reader r(data, size);
    uint8_t type;
    if (!readHeader(r, type)) return;
    if (type != kSealed) {
        handshakeReceive(type, data, size, from);
        return;
    }
    std::vector<uint8_t> plain;
    if (!server_) {
        if (from == serverAddress_ && channel_.open(data, size, 5, plain)) clientReceive(plain.data(), plain.size());
        return;
    }
    auto client = std::find_if(clients_.begin(), clients_.end(), [&](const Client& c) { return c.address == from; });
    if (client != clients_.end() && client->channel.open(data, size, 5, plain)) {
        serverReceive(plain.data(), plain.size(), from);
        return;
    }
    auto p = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& q) { return q.address == from; });
    if (p == pending_.end()) return;
    if (p->channel.open(data, size, 5, plain)) {   // the first packet with new keys: the client joins (or rejoins)
        crypto::Channel fresh = p->channel;
        pending_.erase(p);
        serverReceive(plain.data(), plain.size(), from, &fresh);
        return;
    }
    // Keys that don't match: a wrong password (twice: one could be a packet from before, late; and not from a client
    // that's in: a replay of its packets)
    if (client != clients_.end() || ++p->failures < 2) return;
    pending_.erase(p);
    auto& [count, at] = wrongPasswords_[from.ip];
    count = clock_ - at > 60.0 ? 1 : count + 1;
    at = clock_;
    Writer w(kRefused);
    w.str("wrong password");
    send(from, w.bytes);
    logLine("multiplayer: %s: wrong password", from.str().c_str());
}

void NetSession::handshakeReceive(uint8_t type, const uint8_t* data, size_t size, const net::Address& from) {
    Reader r(data + 5, size - 5);
    if (server_ && type == kOffer && size >= 5 + 32 + 64) {
        auto wrong = wrongPasswords_.find(from.ip);
        if (wrong != wrongPasswords_.end() && wrong->second.first >= kMaxWrongPasswords && clock_ - wrong->second.second < 60.0) {
            Writer w(kRefused);
            w.str("too many wrong passwords: try again in a minute");
            send(from, w.bytes);
            return;
        }
        crypto::Key theirs;
        std::memcpy(theirs.data(), data + 5, 32);
        auto p = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& q) { return q.address == from; });
        if (p == pending_.end() || p->theirs != theirs) {   // (the same offer again: the same answer)
            if (p != pending_.end()) pending_.erase(p);
            if (pending_.size() >= kMaxPending) pending_.erase(pending_.begin());
            Pending q;
            q.address = from;
            q.theirs = theirs;
            q.mine = crypto::KeyPair::generate();
            q.at = clock_;
            if (!q.channel.derive(q.mine, theirs, false, crypto::passwordKey(password_))) return;
            pending_.push_back(std::move(q));
            p = pending_.end() - 1;
        }
        Writer w(kAccept);
        w.bytes.insert(w.bytes.end(), p->mine.publicKey.begin(), p->mine.publicKey.end());
        w.bytes.insert(w.bytes.end(), theirs.begin(), theirs.end());
        send(from, w.bytes);
        return;
    }
    if (server_ || from != serverAddress_) return;
    if (type == kAccept && size >= 5 + 64 && !channel_.ready()) {
        crypto::Key theirs, mine;
        std::memcpy(theirs.data(), data + 5, 32);
        std::memcpy(mine.data(), data + 5 + 32, 32);
        if (mine != keys_.publicKey || !channel_.derive(keys_, theirs, true, crypto::passwordKey(password_))) return;
        heardAt_ = clock_;
        helloAt_ = -1.0;   // (Hello, sealed, at once)
    } else if (type == kRefused && !welcomed_ && !refused_) {
        std::string why = r.str();
        if (!r.ok()) return;
        refused_ = true;
        status_ = "Refused: " + why;
        logLine("multiplayer: %s refused: %s", serverAddress_.str().c_str(), why.c_str());
    }
}

// Keeping the lobby, punching, finding the lobby, and giving up on a direct way
void NetSession::updateRelay() {
    if (!useRelay_) return;
    if (server_ && !lobbyName_.empty() && clock_ - registerAt_ > (lobbyCode_.empty() ? 0.5 : 2.0)) {
        relay::Message m;   // (again every 2 s: the relay keeps the lobby, and its player count, up to date)
        m.type = relay::Type::Register;
        m.token = lobbyToken_;
        m.game = lobbyGame_;
        m.name = lobbyName_;
        m.players = (int)clients_.size() + (dedicated_ ? 0 : 1);
        m.maxPlayers = lobbyMax_;
        m.listed = lobbyListed_;
        m.password = !password_.empty();
        if (relayChallenged_) {
            m.nonce = relayNonce_;
            m.proof = relay::registerProof(relaySecret_, m);
        }
        sendRelay(m);
        registerAt_ = clock_;
    }
    if (!server_ && finding_ && clock_ - connectAt_ > 0.5) {
        relay::Message m;
        m.type = relay::Type::Connect;
        m.code = joinCode_;
        sendRelay(m);
        connectAt_ = clock_;
        if (clock_ > 10.0 && !lobbyFailed_) status_ = "No answer from the relay " + relay_.str();
    }
    punching_.erase(std::remove_if(punching_.begin(), punching_.end(), [&](const auto& p) { return clock_ > p.second; }),
                    punching_.end());
    if (!punching_.empty() && clock_ - punchAt_ > 0.2) {   // (a few: the first may die at the other's NAT)
        relay::Message m;
        m.type = relay::Type::Punch;
        for (const auto& [to, until] : punching_)
            if (!relayed(to)) send(to, relay::encode(m));
        punchAt_ = clock_;
    }
    // A client the server hasn't answered directly in 2 s: through the relay from now on
    if (!server_ && !finding_ && !welcomed_ && serverAddress_.port && !relayed(serverAddress_) && clock_ - peerAt_ > 2.0) {
        relayed_.insert(addressKey(serverAddress_));
        helloAt_ = -1.0;
        status_ = "Joining lobby " + joinCode_ + " through the relay...";
        logLine("multiplayer: no direct way to %s: through the relay", serverAddress_.str().c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// Messages between machines: numbered per peer, sent at once, resent until acknowledged, delivered in order
// ---------------------------------------------------------------------------------------------
void NetSession::queueMessages() {
    for (NetMessage& nm : game_->takeNetMessages()) {
        if (nm.toServer == server_) continue;   // (the server's own to itself: already delivered here)
        Writer w;
        w.put((uint32_t)nm.to);
        w.put((uint32_t)nm.message.from);
        w.put(nm.message.value);
        w.str(nm.message.name.substr(0, 200));
        w.str(nm.message.text.substr(0, 800));
        auto queue = [&](Reliable& r) {
            if (r.unacked.size() >= kMaxUnacked) {
                logLine("multiplayer: too many messages waiting to be acknowledged: \"%s\" dropped", nm.message.name.c_str());
                return;
            }
            r.unacked.push_back({r.next++, w.bytes});
        };
        if (!server_) queue(messages_);
        else
            for (Client& c : clients_)
                if (nm.client == 0 || nm.client == c.id) queue(c.messages);
    }
}

void NetSession::flushMessages(Reliable& r, const net::Address& to, crypto::Channel& channel) {
    bool resend = clock_ >= r.resendAt;
    Writer w(kMessages);
    w.put(r.received);
    size_t countAt = w.bytes.size();
    w.put((uint8_t)0);
    uint8_t count = 0;
    for (const auto& [seq, bytes] : r.unacked) {   // (in order: new ones, and the rest when it's time to resend)
        if (seq <= r.sentUpTo && !resend) continue;
        if (count == 255 || w.bytes.size() + 4 + bytes.size() > kPartBytes) break;
        w.put(seq);
        w.append(bytes);
        r.sentUpTo = std::max(r.sentUpTo, seq);
        ++count;
    }
    if (count == 0 && !r.ackDue) return;
    w.bytes[countAt] = count;
    if (count > 0) r.resendAt = clock_ + 0.2;
    r.ackDue = false;
    sendSealed(to, channel, w.bytes);
}

void NetSession::receiveMessages(Reliable& r, const uint8_t* data, size_t size, uint16_t client) {
    Reader rd(data + 5, size - 5);
    uint32_t ack = rd.get<uint32_t>();
    uint8_t count = rd.get<uint8_t>();
    if (!rd.ok()) return;
    while (!r.unacked.empty() && r.unacked.front().first <= ack) r.unacked.pop_front();
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t seq = rd.get<uint32_t>();
        Message m;
        EntityId to = rd.get<uint32_t>();
        m.from = rd.get<uint32_t>();
        m.value = rd.get<float>();
        m.name = rd.str();
        m.text = rd.str();
        if (!rd.ok()) break;
        if (seq != r.received + 1) continue;   // (seen already, or one before it missing: it comes again)
        r.received = seq;
        m.client = client;
        game_->context().send(to, m);
    }
    if (count > 0) r.ackDue = true;
}

void NetSession::flushDelayed() {
    while (!delayed_.empty() && delayed_.front().at <= clock_) {
        socket_.send(delayed_.front().to, delayed_.front().bytes.data(), delayed_.front().bytes.size());
        delayed_.pop_front();
    }
}

// ---------------------------------------------------------------------------------------------
// Every frame
// ---------------------------------------------------------------------------------------------
void NetSession::update(float dt) {
    if (!socket_.isOpen() || !game_ || !game_->running()) return;
    clock_ += std::max(dt, 0.f);
    flushDelayed();
    uint8_t buf[2048];
    net::Address from;
    for (size_t n; (n = socket_.receive(from, buf, sizeof(buf))) > 0;) {
        if (useRelay_ && from == relay_) relayReceive(buf, n);
        else if (relay::isRelayPacket(buf, n)) continue;   // (a peer's punch: it opened the way, nothing to do)
        else {
            if (relayed(from)) relayed_.erase(addressKey(from));   // (it came straight here: no need for the relay)
            receive(buf, n, from);
        }
    }
    updateRelay();
    if (server_) {
        for (size_t i = clients_.size(); i-- > 0;)
            if (clock_ - clients_[i].heard > kTimeout) dropClient(i, "timed out");
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return clock_ - p.at > kTimeout; }),
                       pending_.end());
        queueMessages();
        for (Client& c : clients_) flushMessages(c.messages, c.address, c.channel);
        snapshotTimer_ += dt;
        if (snapshotTimer_ >= 0.05f) {   // 20 a second
            snapshotTimer_ = 0.f;
            sendSnapshots();
        }
        status_ = std::string(dedicated_ ? "Serving" : "Hosting") + " on port " + std::to_string(socket_.port()) + ": " +
                  std::to_string(clients_.size()) + (clients_.size() == 1 ? " player joined" : " players joined") +
                  (lobbyCode_.empty() ? "" : ", lobby " + lobbyCode_);
    } else {
        if (!welcomed_ && !finding_ && !refused_ && serverAddress_.port && clock_ - helloAt_ > 0.5) {   // (until it answers)
            if (!channel_.ready()) {
                sendOffer();
            } else {
                Writer hello(kHello);
                hello.str("player");
                toServer(hello.bytes);
            }
            helloAt_ = clock_;
        }
        if (welcomed_ && clock_ - pingAt_ > 0.5) {   // the round trip, twice a second
            Writer ping(kPing);
            ping.put(clientId_);
            ping.put(clock_);
            toServer(ping.bytes);
            pingAt_ = clock_;
        }
        if (welcomed_) {
            queueMessages();
            flushMessages(messages_, serverAddress_, channel_);
        }
        if (!finding_ && !lobbyFailed_ && !refused_ && clock_ - heardAt_ > kTimeout)
            status_ = welcomed_ ? "Lost the server" : "No answer from " + serverAddress_.str();
        smooth(dt);
    }
    flushDelayed();
}

// ---------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------
// A client's packet, opened (fresh: the keys of a client joining, or rejoining, with them)
void NetSession::serverReceive(const uint8_t* data, size_t size, const net::Address& from, crypto::Channel* fresh) {
    Reader r(data, size);
    uint8_t type;
    if (!readHeader(r, type)) return;
    auto client = std::find_if(clients_.begin(), clients_.end(), [&](const Client& c) { return c.address == from; });
    if (fresh && type != kHello) return;   // (a client's first sealed packet is its Hello)
    if (type == kHello) {
        if (client != clients_.end() && fresh) client->channel = *fresh;   // (the same address, keys anew: it rejoined)
        if (client == clients_.end()) {   // someone new: a player for them
            if (!fresh) return;
            Client c;
            c.id = nextClient_++;
            c.address = from;
            c.name = r.str();
            c.channel = *fresh;
            c.input = InputMap(game_->scene()->inputActions);
            const NetworkSettings& ns = game_->scene()->network;
            if (!ns.playerPrefab.empty()) {
                float angle = (float)c.id * 2.4f;   // (round the spawn point)
                Vec3 at = ns.spawnPoint + Vec3{std::cos(angle), 0.f, std::sin(angle)} * ns.spawnSpread;
                c.player = game_->context().spawn(ns.playerPrefab, at);
            }
            clients_.push_back(std::move(c));
            client = clients_.end() - 1;
            if (client->player != kNoEntity) game_->setInputSource(client->player, &client->input);
            logLine("multiplayer: %s joined (client %u)", from.str().c_str(), client->id);
        }
        client->heard = clock_;
        Writer w(kWelcome);
        w.put(client->id);
        w.put((uint32_t)client->player);
        w.str(scenePath_);
        sendSealed(from, client->channel, w.bytes);
        return;
    }
    if (client == clients_.end()) return;
    client->heard = clock_;
    if (type == kInput) {
        r.get<uint16_t>();
        uint64_t ack = r.get<uint64_t>();
        float rtt = r.get<float>();
        if (r.ok() && ack > client->acked && client->sent.count(ack)) {   // it has that one whole: the new baseline
            client->acked = ack;
            for (EntityId id : client->sent[ack].gone) client->goneKnown.insert(id);
            client->sent.erase(client->sent.begin(), client->sent.find(ack));
        }
        if (r.ok()) client->rtt = std::clamp(rtt, 0.f, 1.f);
        uint8_t count = r.get<uint8_t>();
        for (uint8_t i = 0; i < count && r.ok(); ++i) {
            Input in;
            in.seq = r.get<uint32_t>();
            uint8_t n = r.get<uint8_t>();
            for (uint8_t k = 0; k < n; ++k) in.values.push_back((float)r.get<uint8_t>() / 255.f);
            in.mouse[0] = r.get<float>();
            in.mouse[1] = r.get<float>();
            in.arrived = clock_;
            if (r.ok() && in.seq > client->received) {   // (each packet repeats the last few: only new ones)
                client->received = in.seq;
                client->pending.push_back(std::move(in));
            }
        }
        while (client->pending.size() > 16) client->pending.pop_front();   // (far behind: catch up)
    } else if (type == kPing) {
        r.get<uint16_t>();
        double t = r.get<double>();
        if (!r.ok()) return;
        Writer pong(kPong);
        pong.put(t);
        sendSealed(from, client->channel, pong.bytes);
    } else if (type == kMessages) {
        receiveMessages(client->messages, data, size, client->id);
    } else if (type == kBye) {
        dropClient((size_t)(client - clients_.begin()), "left");
    }
}

void NetSession::dropClient(size_t index, const char* why) {
    Client& c = clients_[index];
    logLine("multiplayer: client %u %s", c.id, why);
    if (c.player != kNoEntity && game_ && game_->running()) {
        game_->setInputSource(c.player, nullptr);
        game_->context().destroy(c.player);
    }
    clients_.erase(clients_.begin() + (long)index);
}

void NetSession::beforeTick() {
    if (server_) {
        for (Client& c : clients_) {   // the next input each player sent (or the last one, held)
            if (!c.pending.empty()) {
                Input in = std::move(c.pending.front());
                c.pending.pop_front();
                c.last = in.values;
                c.applied = in.seq;
                c.waited = (float)(clock_ - in.arrived);
                c.input.applyRemote(in.values, {in.mouse[0], in.mouse[1]});
            } else {
                c.input.applyRemote(c.last, {0.f, 0.f});
            }
        }
        return;
    }
    if (!welcomed_) return;
    // A client: this update's input, to the server (with the last few, in case some get lost); where its player
    // starts the update (for what this input does to it)
    Input in;
    in.seq = ++inputSeq_;
    in.values = game_->input().values();
    ImVec2 m = game_->input().mouseDelta();
    in.mouse[0] = m.x;
    in.mouse[1] = m.y;
    sentInputs_.push_back(in);
    while (sentInputs_.size() > 8) sentInputs_.pop_front();
    Writer w(kInput);
    w.put(clientId_);
    w.put(lastSnapshot_);
    w.put(rtt_);
    w.put((uint8_t)sentInputs_.size());
    for (const Input& i : sentInputs_) {
        w.put(i.seq);
        w.put((uint8_t)std::min<size_t>(i.values.size(), 255));
        for (size_t k = 0; k < i.values.size() && k < 255; ++k)
            w.put((uint8_t)std::lround(std::clamp(i.values[k], 0.f, 1.f) * 255.f));
        w.put(i.mouse[0]);
        w.put(i.mouse[1]);
    }
    toServer(w.bytes);
    if (const Entity* p = game_->context().find(localPlayer_)) tickStart_ = p->transform.pos();
}

void NetSession::afterTick() {
    if (server_) {
        for (Client& c : clients_) c.input.endFixedStep();
        recordHistory();
        return;
    }
    if (!welcomed_) return;
    if (const Entity* p = game_->context().find(localPlayer_)) {   // what this update's input did to the player
        moves_.push_back({inputSeq_, p->transform.pos() - tickStart_});
        while (moves_.size() > 256) moves_.pop_front();
    }
}

void NetSession::sendSnapshots() {
    if (clients_.empty()) return;
    Scene& scene = *game_->scene();
    ++snapshotTick_;
    auto isPlayer = [&](EntityId id) {
        if (id == localPlayer_) return true;
        for (const Client& c : clients_)
            if (c.player == id) return true;
        return false;
    };
    // Every replicated entity (players are, whatever their prefab says) as a blob: without how to make it, and
    // (for clients that may not have it yet) with
    struct Shared {
        EntityId id;
        Vec3 at;
        bool player;
        std::vector<uint8_t> plain, made;
    };
    std::vector<Shared> shared;
    std::vector<Mat4> worlds = scene.worldMatrices();
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const Entity& e = scene.entities[i];
        bool player = isPlayer(e.id);
        if (!e.replicated && !player) continue;
        std::vector<EntityId> ids;
        const Prefab* prefab = instanceOf(scene, e, ids);
        bool fields = e.replicated && e.replicated->fields && !e.behaviours.empty();
        auto blob = [&](bool withPrefab) {
            Writer b;
            b.put((uint32_t)e.parent);
            b.put((uint8_t)((e.visible ? 1 : 0) | (withPrefab ? 2 : 0) | (fields ? 4 : 0)));
            if (withPrefab) {
                b.str(prefab->name);
                b.put((uint16_t)ids.size());
                for (EntityId id : ids) b.put((uint32_t)id);
            }
            for (float f : e.transform.position) b.put(f);
            for (float f : e.transform.rotation) b.put(f);
            for (float f : e.transform.scale) b.put(f);
            if (fields) {
                b.put((uint8_t)std::min<size_t>(e.behaviours.size(), 255));
                for (size_t k = 0; k < e.behaviours.size() && k < 255; ++k) {
                    const Behaviour& beh = *e.behaviours[k];
                    b.str(beh.typeName());
                    b.str(compact(behaviourToJson(beh)));
                }
            }
            return b.bytes;
        };
        Shared s{e.id, {worlds[i].m[12], worlds[i].m[13], worlds[i].m[14]}, player, blob(false), {}};
        if (prefab) s.made = blob(true);
        shared.push_back(std::move(s));
        known_.insert(e.id);
    }
    // Gone since: destroyed for good (clients keep being told until they say they've heard)
    for (auto it = known_.begin(); it != known_.end();) {
        if (!scene.find(*it)) {
            destroyed_[*it] = snapshotTick_;
            it = known_.erase(it);
        } else {
            ++it;
        }
    }
    std::string state = compact(game_->state().toJson());
    float relevance = scene.network.relevance;

    for (Client& c : clients_) {
        const Sent* base = nullptr;
        if (c.acked) {
            auto it = c.sent.find(c.acked);
            if (it != c.sent.end()) base = &it->second;
        }
        Vec3 viewer = c.player != kNoEntity ? game_->context().worldPosition(c.player) : Vec3{};
        Sent now;
        Writer p;
        p.put(base ? c.acked : (uint64_t)0);
        p.put(c.applied);
        now.state = state;
        if (!base || base->state != state) {
            p.put((uint8_t)1);
            p.str(state);
        } else {
            p.put((uint8_t)0);
        }
        std::vector<const Shared*> relevant;
        for (const Shared& s : shared)
            if (relevance <= 0.f || s.player || c.player == kNoEntity || length(s.at - viewer) <= relevance)
                relevant.push_back(&s);
        p.put((uint16_t)std::min<size_t>(relevant.size(), 65535));
        for (size_t k = 0; k < relevant.size() && k < 65535; ++k) {
            const Shared& s = *relevant[k];
            p.put((uint32_t)s.id);
            auto had = base ? base->blobs.find(s.id) : decltype(base->blobs.end()){};
            bool inBase = base && had != base->blobs.end();
            now.blobs[s.id] = s.plain;
            if (inBase && had->second == s.plain) {
                p.put((uint8_t)0);   // (unchanged: the client has it)
                continue;
            }
            const std::vector<uint8_t>& blob = !inBase && !s.made.empty() ? s.made : s.plain;
            p.put((uint8_t)1);
            p.put((uint16_t)blob.size());
            p.append(blob);
        }
        for (const auto& [id, tick] : destroyed_)
            if (!c.goneKnown.count(id) && now.gone.size() < 512) now.gone.push_back(id);
        p.put((uint16_t)now.gone.size());
        for (EntityId id : now.gone) p.put((uint32_t)id);
        c.sent[snapshotTick_] = std::move(now);
        while (c.sent.size() > 64) c.sent.erase(c.sent.begin());   // (it's fallen far behind: full ones again)
        // In parts
        size_t parts = std::max<size_t>(1, (p.bytes.size() + kPartBytes - 1) / kPartBytes);
        if (parts > 255) continue;   // (too big to send: the next one will be smaller, as a delta)
        for (size_t k = 0; k < parts; ++k) {
            size_t from = k * kPartBytes, n = std::min(kPartBytes, p.bytes.size() - from);
            Writer w(kSnapshot);
            w.put(snapshotTick_);
            w.put((uint8_t)k);
            w.put((uint8_t)parts);
            w.put((uint16_t)n);
            w.bytes.insert(w.bytes.end(), p.bytes.begin() + (long)from, p.bytes.begin() + (long)(from + n));
            sendSealed(c.address, c.channel, w.bytes);
        }
    }
}

// Lag compensation: the last second of where every replicated (or player) entity with a collider was
void NetSession::recordHistory() {
    if (!lagCompensation_ || clients_.empty()) return;
    Scene& scene = *game_->scene();
    Pose pose;
    pose.time = game_->time();
    for (const Entity& e : scene.entities) {
        if (!e.collider) continue;
        bool player = e.id == localPlayer_ || std::any_of(clients_.begin(), clients_.end(), [&](const Client& c) { return c.player == e.id; });
        if (e.replicated || player) pose.transforms[e.id] = e.transform;
    }
    history_.push_back(std::move(pose));
    while (!history_.empty() && pose.time - history_.front().time > 1.f) history_.pop_front();
    while (history_.size() > 2 && history_.back().time - history_.front().time > 1.f) history_.pop_front();
}

void NetSession::rewind(EntityId player) {
    rewound_.clear();
    if (!lagCompensation_ || history_.empty()) return;
    auto c = std::find_if(clients_.begin(), clients_.end(), [&](const Client& k) { return k.player == player; });
    if (c == clients_.end()) return;
    // Where that player saw the world when it gave this input: its round trip, the input's wait here and the
    // client's smoothing ago
    float when = game_->time() - (c->rtt + c->waited + kSmoothingDelay);
    const Pose* pose = &history_.front();
    for (const Pose& p : history_)
        if (p.time <= when) pose = &p;
    Scene& scene = *game_->scene();
    for (const auto& [id, t] : pose->transforms) {
        if (id == player || scene.isAncestor(player, id)) continue;
        Entity* e = scene.find(id);
        if (!e) continue;
        rewound_[id] = e->transform;
        e->transform = t;
    }
    if (!rewound_.empty()) game_->syncPhysics();
}

void NetSession::unrewind() {
    if (rewound_.empty()) return;
    Scene& scene = *game_->scene();
    for (const auto& [id, t] : rewound_)
        if (Entity* e = scene.find(id)) e->transform = t;
    rewound_.clear();
    game_->syncPhysics();
}

// ---------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------
void NetSession::clientReceive(const uint8_t* data, size_t size) {
    Reader r(data, size);
    uint8_t type;
    if (!readHeader(r, type)) return;
    heardAt_ = clock_;
    if (type == kWelcome) {
        clientId_ = r.get<uint16_t>();
        localPlayer_ = r.get<uint32_t>();
        std::string scene = r.str();
        if (!r.ok()) return;
        if (!welcomed_) {
            logLine("multiplayer: joined %s as client %u", serverAddress_.str().c_str(), clientId_);
            if (!scene.empty() && scene != game_->scenePath()) game_->context().loadScene(scene);   // (the server's)
        }
        welcomed_ = true;
        game_->setLocalPlayer(localPlayer_);
        status_ = "Playing on " + serverAddress_.str() + (relayed(serverAddress_) ? " (through the relay)" : "");
        return;
    }
    if (type == kPong) {
        double t = r.get<double>();
        if (!r.ok()) return;
        float sample = (float)(clock_ - t);
        rtt_ = rtt_ <= 0.f ? sample : rtt_ + (sample - rtt_) * 0.3f;
        return;
    }
    if (type == kBye) {
        status_ = "The server left";
        welcomed_ = false;
        return;
    }
    if (type == kMessages) {
        if (welcomed_) receiveMessages(messages_, data, size, 0);
        return;
    }
    if (type != kSnapshot || !welcomed_) return;
    uint64_t tick = r.get<uint64_t>();
    uint8_t part = r.get<uint8_t>(), parts = r.get<uint8_t>();
    uint16_t n = r.get<uint16_t>();
    if (!r.ok() || parts == 0 || part >= parts || tick <= lastSnapshot_ || size < 5 + 12 + (size_t)n) return;
    Partial& p = partials_[tick];
    if (p.parts.empty()) p.parts.resize(parts);
    if (p.parts.size() != parts || !p.parts[part].empty()) return;
    p.parts[part].assign(data + 5 + 12, data + 5 + 12 + n);
    if (++p.have < parts) return;
    std::vector<uint8_t> payload;   // whole: put together, used, and the ones before it given up on
    for (const std::vector<uint8_t>& b : p.parts) payload.insert(payload.end(), b.begin(), b.end());
    partials_.erase(partials_.begin(), partials_.upper_bound(tick));
    applySnapshot(tick, payload);
}

void NetSession::applySnapshot(uint64_t tick, const std::vector<uint8_t>& payload) {
    Reader r(payload.data(), payload.size());
    uint64_t baseTick = r.get<uint64_t>();
    uint32_t applied = r.get<uint32_t>();
    const Got* base = nullptr;
    if (baseTick) {
        auto it = got_.find(baseTick);
        if (it == got_.end()) return;   // (a delta from one this client no longer has: wait for the next)
        base = &it->second;
    }
    Got now;
    if (r.get<uint8_t>() == 1) {
        now.state = r.str();
        json::Value state;
        std::string error;
        if (r.ok() && json::parse(now.state, state, error)) game_->state().fromJson(state);
    } else if (base) {
        now.state = base->state;
    }
    if (!r.ok()) return;
    uint16_t count = r.get<uint16_t>();
    std::vector<std::pair<EntityId, const std::vector<uint8_t>*>> entities;
    for (uint16_t i = 0; i < count && r.ok(); ++i) {
        EntityId id = r.get<uint32_t>();
        if (r.get<uint8_t>() == 1) {
            uint16_t size = r.get<uint16_t>();
            std::vector<uint8_t> blob(size);
            for (uint16_t k = 0; k < size; ++k) blob[k] = r.get<uint8_t>();
            now.blobs[id] = std::move(blob);
        } else if (base && base->blobs.count(id)) {
            now.blobs[id] = base->blobs.at(id);
        }
    }
    std::vector<EntityId> gone;
    uint16_t goneCount = r.get<uint16_t>();
    for (uint16_t i = 0; i < goneCount && r.ok(); ++i) gone.push_back(r.get<uint32_t>());
    if (!r.ok()) return;
    lastSnapshot_ = tick;
    if (!marked_ || markedPath_ != game_->scenePath()) {
        // The first snapshot in this scene: everything the server shares is the server's (not simulated here),
        // shown only as it sends it (what's out of this client's interest stays hidden)
        Scene& scene = *game_->scene();
        for (Entity& e : scene.entities) {
            if (!e.replicated || e.id == localPlayer_) continue;
            game_->setSimulated(e.id, false);
            for (Entity& c : scene.entities)
                if (scene.isAncestor(e.id, c.id)) game_->setSimulated(c.id, false);
            if (e.rigidBody) e.rigidBody->kinematic = true;
            if (!now.blobs.count(e.id)) e.visible = false;
        }
        marked_ = true;
        markedPath_ = game_->scenePath();
    }
    for (const auto& [id, blob] : now.blobs) applyEntity(id, blob);
    for (EntityId id : gone) {
        if (game_->context().find(id) && !game_->context().isDestroyed(id)) game_->context().destroy(id);
        targets_.erase(id);
    }
    // Interest: what it showed last time and no longer gets (not destroyed) is out of range: hidden
    for (EntityId id : shown_)
        if (!now.blobs.count(id) && std::find(gone.begin(), gone.end(), id) == gone.end())
            if (Entity* e = game_->context().find(id); e && id != localPlayer_) e->visible = false;
    shown_.clear();
    for (const auto& [id, blob] : now.blobs) shown_.insert(id);
    // The player: where the server has it after input `applied`
    if (auto it = now.blobs.find(localPlayer_); it != now.blobs.end() && it->second.size() >= 5 + 36) {
        Transform t;
        Reader b(it->second.data(), it->second.size());
        b.get<uint32_t>();
        uint8_t flags = b.get<uint8_t>();
        if (flags & 2) {
            b.str();
            uint16_t k = b.get<uint16_t>();
            for (uint16_t j = 0; j < k; ++j) b.get<uint32_t>();
        }
        for (float& f : t.position) f = b.get<float>();
        if (b.ok()) reconcile(applied, t);
    }
    got_[tick] = std::move(now);
    while (got_.size() > 32) got_.erase(got_.begin());
}

// Prediction: the server's word for where the player was after input `applied`, plus what the inputs since did here
void NetSession::reconcile(uint32_t applied, const Transform& server) {
    while (!moves_.empty() && moves_.front().seq <= applied) moves_.pop_front();
    Entity* e = game_->context().find(localPlayer_);
    if (!e || applied == 0) return;
    Vec3 predicted = server.pos();
    for (const Move& m : moves_) predicted = predicted + m.delta;
    Vec3 here = e->transform.pos();
    if (length(predicted - here) < 0.01f) return;   // (agrees)
    if (length(predicted - here) > 0.05f) ++corrections_;
    for (int k = 0; k < 3; ++k) e->transform.position[k] = k == 0 ? predicted.x : k == 1 ? predicted.y : predicted.z;
}

void NetSession::applyEntity(EntityId id, const std::vector<uint8_t>& blob) {
    Scene& scene = *game_->scene();
    GameContext& ctx = game_->context();
    Reader r(blob.data(), blob.size());
    EntityId parent = r.get<uint32_t>();
    uint8_t flags = r.get<uint8_t>();
    std::string prefabName;
    std::vector<EntityId> ids;
    if (flags & 2) {
        prefabName = r.str();
        uint16_t n = r.get<uint16_t>();
        for (uint16_t k = 0; k < n; ++k) ids.push_back(r.get<uint32_t>());
    }
    Transform t;
    for (float& f : t.position) f = r.get<float>();
    for (float& f : t.rotation) f = r.get<float>();
    for (float& f : t.scale) f = r.get<float>();
    std::vector<std::pair<std::string, std::string>> fields;
    if (flags & 4) {
        uint8_t n = r.get<uint8_t>();
        for (uint8_t k = 0; k < n; ++k) {
            std::string type = r.str();
            fields.emplace_back(type, r.str());
        }
    }
    if (!r.ok()) return;

    bool mine = id == localPlayer_;
    Entity* e = ctx.find(id);
    if (!e && !prefabName.empty())   // made on the server since: made here, with the server's ids
        if (const Prefab* p = scene.findPrefab(prefabName); p && ids.size() == p->entities.size() && ids[0] == id) {
            std::vector<Entity> copies = scene.instantiate(*p, parent);
            std::map<EntityId, EntityId> remap;
            for (size_t k = 0; k < copies.size(); ++k) remap[copies[k].id] = ids[k];
            for (Entity& c : copies) {
                c.id = remap[c.id];
                if (remap.count(c.parent)) c.parent = remap[c.parent];
            }
            copies[0].transform = t;
            for (Entity& c : copies)
                if (!mine) {   // (someone else's: moved by the server, not simulated here)
                    if (c.rigidBody) c.rigidBody->kinematic = true;
                    game_->setSimulated(c.id, false);
                }
            game_->addEntities(std::move(copies));
            e = ctx.find(id);
        }
    if (!e || mine) return;   // (the player: predicted here, put right by reconcile())
    // Someone else's: shown where the server has it (smoothed), never simulated here
    if (game_->simulated(id)) {
        game_->setSimulated(id, false);
        for (Entity& c : scene.entities)
            if (scene.isAncestor(id, c.id)) game_->setSimulated(c.id, false);
        if (e->rigidBody) e->rigidBody->kinematic = true;
        e->transform = t;
    }
    if (!e->visible && (flags & 1)) e->transform = t;   // (back in view: there at once, not slid in)
    e->visible = (flags & 1) != 0;
    Target& target = targets_[id];
    target.position = t.pos();
    target.rotation = rotationOf(t.matrix());
    target.scale = {t.scale[0], t.scale[1], t.scale[2]};
    for (size_t k = 0; k < fields.size() && k < e->behaviours.size(); ++k) {
        if (!e->behaviours[k] || fields[k].first != e->behaviours[k]->typeName()) continue;
        json::Value v;
        std::string error;
        if (json::parse(fields[k].second, v, error)) readBehaviourFields(*e->behaviours[k], v);
    }
}

void NetSession::smooth(float dt) {
    float k = 1.f - std::exp(-15.f * std::max(dt, 0.f));
    for (auto it = targets_.begin(); it != targets_.end();) {
        Entity* e = game_->context().find(it->first);
        if (!e) { it = targets_.erase(it); continue; }
        Vec3 p = e->transform.pos();
        Quat q = rotationOf(e->transform.matrix());
        Vec3 pos = p + (it->second.position - p) * k;
        Quat rot = slerp(q, it->second.rotation, k);
        e->transform.setMatrix(composeTRS(pos, rot, it->second.scale));
        ++it;
    }
}
