#include "net/net_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "scene/serialization.h"

// Packets: "VKN1", a type, then the type's fields (little-endian)
//   Hello    (client -> server)  name
//   Welcome  (server -> client)  client id (u16), its player (u32)
//   Input    (client -> server)  client id, count (u8), then each: seq (u32), values (u8 count, u8 each), mouse (2 f32)
//   Snapshot (server -> client)  tick (u64), the client's last input applied (u32), state (flag, json), entities
//                                (u16 count, each: see writeEntity), destroyed ids (u16 count, u32 each)
//   Bye      (both)              client id
namespace {

constexpr uint8_t kMagic[4] = {'V', 'K', 'N', '1'};
enum : uint8_t { kHello = 1, kWelcome, kInput, kSnapshot, kBye };
constexpr size_t kMaxPacket = 1200;    // (fits any network's packets without splitting)
constexpr EntityId kClientIds = 1u << 23;   // a client's own entities: ids from here (the server's are lower)
constexpr int kAnnounce = 40;          // snapshots a spawned entity carries how to make it (2 s)
constexpr double kTimeout = 10.0;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

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
}

bool NetSession::host(Game& game, uint16_t port, std::string& error) {
    stop();
    if (!game.running()) { error = "start the game first"; return false; }
    if (!socket_.open(port, error)) return false;
    game_ = &game;
    server_ = true;
    clock_ = now();
    hookGame();
    // The host plays too: its own player from the prefab, if the scene names one
    Scene& scene = *game.scene();
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
    clock_ = heardAt_ = now();
    helloAt_ = -1.0;
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
    if (game_) game_->setTickHooks(nullptr, nullptr);
    socket_.close();
    welcomed_ = false;
    targets_.clear();
    status_ = "Offline";
}

// ---------------------------------------------------------------------------------------------
// Every frame
// ---------------------------------------------------------------------------------------------
void NetSession::update(float dt) {
    if (!socket_.isOpen() || !game_ || !game_->running()) return;
    clock_ = now();
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
            socket_.send(serverAddress_, hello.bytes.data(), hello.bytes.size());
            helloAt_ = clock_;
        }
        if (clock_ - heardAt_ > kTimeout) status_ = welcomed_ ? "Lost the server" : "No answer from " + serverAddress_.str();
        smooth(dt);
    }
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
            sentLast_.clear();   // (everyone gets how to make what they see again: the newcomer has nothing)
            logLine("multiplayer: %s joined (client %u)", from.str().c_str(), client->id);
        }
        client->heard = clock_;
        Writer w(kWelcome);
        w.put(client->id);
        w.put((uint32_t)client->player);
        socket_.send(from, w.bytes.data(), w.bytes.size());
        return;
    }
    if (client == clients_.end()) return;
    client->heard = clock_;
    if (type == kInput) {
        r.get<uint16_t>();
        uint8_t count = r.get<uint8_t>();
        for (uint8_t i = 0; i < count && r.ok(); ++i) {
            Input in;
            in.seq = r.get<uint32_t>();
            uint8_t n = r.get<uint8_t>();
            for (uint8_t k = 0; k < n; ++k) in.values.push_back((float)r.get<uint8_t>() / 255.f);
            in.mouse[0] = r.get<float>();
            in.mouse[1] = r.get<float>();
            if (r.ok() && in.seq > client->received) {   // (each packet repeats the last few: only new ones)
                client->received = in.seq;
                client->pending.push_back(std::move(in));
            }
        }
        while (client->pending.size() > 16) client->pending.pop_front();   // (far behind: catch up)
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
                c.input.applyRemote(in.values, {in.mouse[0], in.mouse[1]});
            } else {
                c.input.applyRemote(c.last, {0.f, 0.f});
            }
        }
        return;
    }
    if (!welcomed_) return;
    // A client: this update's input, to the server (with the last few, in case some get lost)
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
    w.put((uint8_t)sentInputs_.size());
    for (const Input& i : sentInputs_) {
        w.put(i.seq);
        w.put((uint8_t)std::min<size_t>(i.values.size(), 255));
        for (size_t k = 0; k < i.values.size() && k < 255; ++k)
            w.put((uint8_t)std::lround(std::clamp(i.values[k], 0.f, 1.f) * 255.f));
        w.put(i.mouse[0]);
        w.put(i.mouse[1]);
    }
    socket_.send(serverAddress_, w.bytes.data(), w.bytes.size());
}

void NetSession::afterTick() {
    if (server_)
        for (Client& c : clients_) c.input.endFixedStep();
}

void NetSession::sendSnapshots() {
    if (clients_.empty()) return;
    Scene& scene = *game_->scene();
    ++snapshotTick_;
    // Every replicated entity (players are, whatever their prefab says), each as its own blob
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<EntityId> sent;
    if (sentLast_.empty()) announced_.clear();
    auto isPlayer = [&](EntityId id) {
        if (id == localPlayer_) return true;
        for (const Client& c : clients_)
            if (c.player == id) return true;
        return false;
    };
    for (const Entity& e : scene.entities) {
        if (!e.replicated && !isPlayer(e.id)) continue;
        Writer b;
        b.put((uint32_t)e.id);
        b.put((uint32_t)e.parent);
        std::vector<EntityId> ids;
        const Prefab* prefab = announced_[e.id] < kAnnounce ? instanceOf(scene, e, ids) : nullptr;
        bool fields = e.replicated && e.replicated->fields && !e.behaviours.empty();
        b.put((uint8_t)((e.visible ? 1 : 0) | (prefab ? 2 : 0) | (fields ? 4 : 0)));
        if (prefab) {
            b.str(prefab->name);
            b.put((uint16_t)ids.size());
            for (EntityId id : ids) b.put((uint32_t)id);
        }
        for (float f : e.transform.position) b.put(f);
        for (float f : e.transform.rotation) b.put(f);
        for (float f : e.transform.scale) b.put(f);
        if (fields) {
            b.put((uint8_t)std::min<size_t>(e.behaviours.size(), 255));
            for (size_t i = 0; i < e.behaviours.size() && i < 255; ++i) {
                const Behaviour& beh = *e.behaviours[i];
                b.str(beh.typeName());
                b.str(compact(behaviourToJson(beh)));
            }
        }
        ++announced_[e.id];
        blobs.push_back(std::move(b.bytes));
        sent.push_back(e.id);
    }
    for (EntityId id : sentLast_)   // gone since: announced for a while (packets get lost)
        if (std::find(sent.begin(), sent.end(), id) == sent.end()) gone_[id] = 20;
    sentLast_ = sent;
    std::string state = compact(game_->state().toJson());

    for (const Client& c : clients_) {
        size_t next = 0;
        bool first = true;
        do {   // as many packets as it takes
            Writer w(kSnapshot);
            w.put(snapshotTick_);
            w.put(c.applied);
            w.put((uint8_t)(first ? 1 : 0));
            if (first) w.str(state);
            size_t countAt = w.bytes.size();
            w.put((uint16_t)0);
            uint16_t count = 0;
            while (next < blobs.size() && (w.bytes.size() + blobs[next].size() + 64 < kMaxPacket || count == 0)) {
                w.append(blobs[next++]);
                ++count;
            }
            std::memcpy(&w.bytes[countAt], &count, 2);
            std::vector<EntityId> goneNow;
            if (first)
                for (const auto& [id, left] : gone_) goneNow.push_back(id);
            w.put((uint16_t)goneNow.size());
            for (EntityId id : goneNow) w.put((uint32_t)id);
            socket_.send(c.address, w.bytes.data(), w.bytes.size());
            first = false;
        } while (next < blobs.size());
    }
    for (auto it = gone_.begin(); it != gone_.end();) it = --it->second <= 0 ? gone_.erase(it) : std::next(it);
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
        if (!r.ok()) return;
        if (!welcomed_) logLine("multiplayer: joined %s as client %u", serverAddress_.str().c_str(), clientId_);
        welcomed_ = true;
        game_->setLocalPlayer(localPlayer_);
        status_ = "Playing on " + serverAddress_.str();
        return;
    }
    if (type == kBye) {
        status_ = "The server left";
        welcomed_ = false;
        return;
    }
    if (type != kSnapshot || !welcomed_) return;
    uint64_t tick = r.get<uint64_t>();
    r.get<uint32_t>();   // (the last input the server applied: for replaying, not used yet)
    uint8_t flags = r.get<uint8_t>();
    if (!r.ok() || tick < lastSnapshot_) return;   // (an old one, overtaken)
    lastSnapshot_ = tick;
    if (flags & 1) {
        json::Value state;
        std::string error;
        if (json::parse(r.str(), state, error)) game_->state().fromJson(state);
    }
    uint16_t count = r.get<uint16_t>();
    for (uint16_t i = 0; i < count && r.ok(); ++i) applyEntity(r, tick);
    uint16_t gone = r.get<uint16_t>();
    for (uint16_t i = 0; i < gone && r.ok(); ++i) {
        EntityId id = r.get<uint32_t>();
        if (game_->context().find(id) && !game_->context().isDestroyed(id)) game_->context().destroy(id);
        targets_.erase(id);
    }
}

void NetSession::applyEntity(Reader& r, uint64_t) {
    Scene& scene = *game_->scene();
    GameContext& ctx = game_->context();
    EntityId id = r.get<uint32_t>(), parent = r.get<uint32_t>();
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
    if (!e) return;
    if (mine) {   // predicted here: only put right when the server disagrees by much
        Vec3 server = t.pos(), local = e->transform.pos();
        if (length(server - local) > 1.f)
            for (int k = 0; k < 3; ++k) e->transform.position[k] = t.position[k];
        return;
    }
    // Someone else's: shown where the server has it (smoothed), never simulated here
    if (game_->simulated(id)) {
        game_->setSimulated(id, false);
        for (Entity& c : scene.entities)
            if (scene.isAncestor(id, c.id)) game_->setSimulated(c.id, false);
        if (e->rigidBody) e->rigidBody->kinematic = true;
        e->transform = t;
    }
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
