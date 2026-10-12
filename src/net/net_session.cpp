#include "net/net_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "scene/serialization.h"

// Packets: "VKN2", a type, then the type's fields (little-endian)
//   Hello    (client -> server)  name
//   Welcome  (server -> client)  client id (u16), its player (u32), the server's scene (string)
//   Input    (client -> server)  client id, the last snapshot it got whole (u64), its round trip (f32), count (u8),
//                                then each: seq (u32), values (u8 count, u8 each), mouse (2 f32)
//   Snapshot (server -> client)  tick (u64), part (u8), parts (u8), bytes (u16 count, then them): the parts put
//                                together are the payload below
//   Bye      (both)              client id
//   Ping     (client -> server)  client id, its clock (f64); Pong (server -> client): the same clock back
// A snapshot's payload: its baseline (u64: the snapshot it's a delta from, 0: none), the client's last input applied
// (u32), state (u8 flag: 1 = the game state's JSON follows, 0 = as in the baseline), entities (u16 count; each: id
// (u32), u8: 1 = its blob follows (u16 size, bytes), 0 = as in the baseline), destroyed ids (u16 count, u32 each).
// An entity's blob: parent (u32), flags (u8: 1 visible, 2 prefab follows, 4 fields follow), [prefab name, entity
// count (u16), their ids (u32)], position, rotation, scale (9 f32), [behaviour count (u8), each: type, fields JSON]
namespace {

constexpr uint8_t kMagic[4] = {'V', 'K', 'N', '2'};
enum : uint8_t { kHello = 1, kWelcome, kInput, kSnapshot, kBye, kPing, kPong };
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

bool NetSession::host(Game& game, uint16_t port, std::string& error) {
    stop();
    if (!game.running()) { error = "start the game first"; return false; }
    if (!socket_.open(port, error)) return false;
    game_ = &game;
    server_ = true;
    clock_ = 0.0;
    hookGame();
    // What clients may have of this scene (their own copy of it): its entities, gone once destroyed here
    Scene& scene = *game.scene();
    known_.clear();
    destroyed_.clear();
    for (const Entity& e : scene.entities) known_.insert(e.id);
    scenePath_ = game.scenePath();
    // The host plays too: its own player from the prefab, if the scene names one
    if (!scene.network.playerPrefab.empty()) {
        localPlayer_ = game.context().spawn(scene.network.playerPrefab, scene.network.spawnPoint);
        game.setLocalPlayer(localPlayer_);
    }
    status_ = "Hosting on port " + std::to_string(socket_.port());
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
    game.scene()->setNextId(kClientIds);   // (this machine's own entities stay clear of the server's ids)
    hookGame();
    status_ = "Joining " + serverAddress_.str() + "...";
    return true;
}

void NetSession::stop() {
    if (!socket_.isOpen()) return;
    Writer bye(kBye);
    bye.put(clientId_);
    if (server_) {
        for (const Client& c : clients_) socket_.send(c.address, bye.bytes.data(), bye.bytes.size());
        while (!clients_.empty()) dropClient(clients_.size() - 1, "the server stopped");
    } else {
        socket_.send(serverAddress_, bye.bytes.data(), bye.bytes.size());
    }
    if (game_) {
        game_->setTickHooks(nullptr, nullptr);
        game_->setRewindHooks(nullptr, nullptr);
    }
    socket_.close();
    welcomed_ = false;
    targets_.clear();
    partials_.clear();
    got_.clear();
    moves_.clear();
    delayed_.clear();
    history_.clear();
    status_ = "Offline";
}

void NetSession::simulateNetwork(float latency, float loss) {
    latency_ = std::max(latency, 0.f);
    loss_ = std::clamp(loss, 0.f, 1.f);
}

// Every packet goes through here: counted, and (when simulating a network) delayed or lost
void NetSession::send(const net::Address& to, const std::vector<uint8_t>& bytes) {
    bytesSent_ += bytes.size();
    if (loss_ > 0.f && std::uniform_real_distribution<float>(0.f, 1.f)(lossRng_) < loss_) return;
    if (latency_ > 0.f) delayed_.push_back({clock_ + latency_, to, bytes});
    else socket_.send(to, bytes.data(), bytes.size());
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
        if (server_) serverReceive(buf, n, from);
        else if (from == serverAddress_) clientReceive(buf, n);
    }
    if (server_) {
        for (size_t i = clients_.size(); i-- > 0;)
            if (clock_ - clients_[i].heard > kTimeout) dropClient(i, "timed out");
        snapshotTimer_ += dt;
        if (snapshotTimer_ >= 0.05f) {   // 20 a second
            snapshotTimer_ = 0.f;
            sendSnapshots();
        }
        status_ = "Hosting on port " + std::to_string(socket_.port()) + ": " + std::to_string(clients_.size()) +
                  (clients_.size() == 1 ? " player joined" : " players joined");
    } else {
        if (!welcomed_ && clock_ - helloAt_ > 0.5) {   // (until the server answers)
            Writer hello(kHello);
            hello.str("player");
            send(serverAddress_, hello.bytes);
            helloAt_ = clock_;
        }
        if (welcomed_ && clock_ - pingAt_ > 0.5) {   // the round trip, twice a second
            Writer ping(kPing);
            ping.put(clientId_);
            ping.put(clock_);
            send(serverAddress_, ping.bytes);
            pingAt_ = clock_;
        }
        if (clock_ - heardAt_ > kTimeout) status_ = welcomed_ ? "Lost the server" : "No answer from " + serverAddress_.str();
        smooth(dt);
    }
    flushDelayed();
}

// ---------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------
void NetSession::serverReceive(const uint8_t* data, size_t size, const net::Address& from) {
    Reader r(data, size);
    uint8_t type;
    if (!readHeader(r, type)) return;
    auto client = std::find_if(clients_.begin(), clients_.end(), [&](const Client& c) { return c.address == from; });
    if (type == kHello) {
        if (client == clients_.end()) {   // someone new: a player for them
            Client c;
            c.id = nextClient_++;
            c.address = from;
            c.name = r.str();
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
        send(from, w.bytes);
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
        send(from, pong.bytes);
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
    send(serverAddress_, w.bytes);
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
            send(c.address, w.bytes);
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
        status_ = "Playing on " + serverAddress_.str();
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
