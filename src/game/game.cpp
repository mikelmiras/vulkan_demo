#include "game/game.h"

#include "physics/dynamics.h"
#include "game/animation.h"
#include "core/project.h"
#include "scene/serialization.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// GameState
// ---------------------------------------------------------------------------------------------
float GameState::number(const std::string& key, float fallback) const {
    auto it = values_.find(key);
    return it != values_.end() ? it->second.asFloat(fallback) : fallback;
}

std::string GameState::text(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it != values_.end() && it->second.type() == json::Value::Type::String ? it->second.asString() : fallback;
}

bool GameState::flag(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    return it != values_.end() ? it->second.asBool(fallback) : fallback;
}

std::string GameState::format(const std::string& text) const {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        size_t close = text[i] == '{' ? text.find('}', i) : std::string::npos;
        if (close == std::string::npos) {
            out += text[i++];
            continue;
        }
        auto it = values_.find(text.substr(i + 1, close - i - 1));
        if (it != values_.end()) {
            const json::Value& v = it->second;
            if (v.type() == json::Value::Type::String) out += v.asString();
            else if (v.type() == json::Value::Type::Bool) out += v.asBool(false) ? "yes" : "no";
            else {
                double n = v.asNumber(0);
                char buf[32];
                std::snprintf(buf, sizeof(buf), std::fabs(n - std::round(n)) < 1e-4 ? "%.0f" : "%.1f", n);
                out += buf;
            }
        }
        i = close + 1;
    }
    return out;
}

json::Value GameState::toJson() const {
    json::Value v = json::Value::object();
    for (const auto& [key, value] : values_) v.set(key, value);
    return v;
}

void GameState::fromJson(const json::Value& v) {
    values_.clear();
    for (const auto& [key, value] : v.members()) values_[key] = value;
}

// ---------------------------------------------------------------------------------------------
// GameContext
// ---------------------------------------------------------------------------------------------
Vec3 GameContext::worldPosition(EntityId id) const {
    Mat4 m = scene_->worldMatrix(id);
    return {m.m[12], m.m[13], m.m[14]};
}

Entity* GameContext::find(EntityId id) {
    if (Entity* e = scene_->find(id)) return e;
    for (Entity& e : spawnQueue_)
        if (e.id == id) return &e;
    return nullptr;
}

Entity* GameContext::find(const std::string& name) {
    if (Entity* e = scene_->findByName(name)) return e;
    for (Entity& e : spawnQueue_)
        if (e.name == name) return &e;
    return nullptr;
}

EntityId GameContext::spawn(const std::string& prefab, Vec3 position, Vec3 forward, EntityId parent) {
    const Prefab* p = scene_->findPrefab(prefab);
    if (!p || p->entities.empty()) {
        log("spawn: no prefab named '" + prefab + "'");
        return kNoEntity;
    }
    std::vector<Entity> copies = scene_->instantiate(*p, parent);
    Transform& root = copies[0].transform;
    const float unit[3] = {1, 1, 1};
    Mat4 world = lookRotation(position, forward, unit) * root.matrix();
    root.setMatrix(inverseAffine(scene_->worldMatrix(parent)) * world);
    EntityId id = copies[0].id;
    for (Entity& e : copies) spawnQueue_.push_back(std::move(e));
    return id;
}

CharacterMove GameContext::moveCharacter(Entity& self, Vec3 delta) {
    CharacterMove result;
    result.displacement = delta;
    collision::Capsule capsule;
    const PhysicsWorld::Body* body = physics_->find(self.id);
    if (body && PhysicsWorld::capsuleOf(*body, capsule)) {
        // The snapshot is from the start of the update: bring the capsule to where the entity is now
        Mat4 now = scene_->worldMatrix(self.id);
        Vec3 shift = Vec3{now.m[12], now.m[13], now.m[14]} - Vec3{body->world.m[12], body->world.m[13], body->world.m[14]};
        capsule.a += shift;
        capsule.b += shift;
        result = physics_->moveCapsule(capsule, delta, self.id);
        // Push the rigid bodies it walked into, up to its own speed (lighter ones more easily)
        Vec3 velocity = delta * (dt_ > 0.f ? 1.f / dt_ : 0.f);
        for (const auto& [id, into] : result.touched) {
            Entity* pushed = scene_->find(id);
            if (!pushed || !pushed->rigidBody) continue;
            Vec3 dir = normalize(Vec3{into.x, 0.f, into.z});
            float want = dot(velocity, dir), have = dot(pushed->rigidBody->velocity, dir);
            if (want > have) pushed->rigidBody->velocity += dir * ((want - have) * std::min(1.f, 2.f / pushed->rigidBody->mass));
        }
    }
    // World-space displacement -> the entity's parent space
    Vec3 local = transformDir(inverseAffine(scene_->parentWorldMatrix(self.id)), result.displacement);
    self.transform.position[0] += local.x;
    self.transform.position[1] += local.y;
    self.transform.position[2] += local.z;
    return result;
}

void GameContext::playSound(const std::string& clip, Vec3 position, float volume) {
    if (!audio_) return;
    AudioEngine::PlayParams p;
    p.volume = volume;
    p.spatial = true;
    p.position = position;
    audio_->play(clip, p);
}

void GameContext::playSound2D(const std::string& clip, float volume) {
    if (!audio_) return;
    AudioEngine::PlayParams p;
    p.volume = volume;
    audio_->play(clip, p);
}

void GameContext::destroy(EntityId id) {
    if (!isDestroyed(id)) destroyQueue_.push_back(id);
}

bool GameContext::isDestroyed(EntityId id) const {
    return std::find(destroyQueue_.begin(), destroyQueue_.end(), id) != destroyQueue_.end();
}

void GameContext::loadScene(const std::string& path) { game_->pendingScene_ = path; }
void GameContext::setPaused(bool paused) { game_->paused_ = paused; }
bool GameContext::paused() const { return game_->paused_; }
void GameContext::quit() { game_->quit_ = true; }
GameState& GameContext::state() { return game_->state_; }
void GameContext::saveGame(const std::string& slot) { game_->pendingSave_ = slot; }
void GameContext::loadGame(const std::string& slot) { game_->pendingLoad_ = slot; }
const std::string& GameContext::scenePath() const { return game_->scenePath_; }

bool GameContext::hasSaveGame(const std::string& slot) const {
    std::error_code ec;
    return fs::is_regular_file(game_->saveGamePath(slot), ec);
}

void GameContext::log(const std::string& message) {
    std::printf("[game %7.2fs] %s\n", time_, message.c_str());
    std::fflush(stdout);
    log_->emplace_back(time_, message);
    if (log_->size() > 100) log_->erase(log_->begin());
}

// ---------------------------------------------------------------------------------------------
// Game
// ---------------------------------------------------------------------------------------------
void Game::start(Scene& scene, const MeshLibrary& meshes) {
    scene_ = &scene;
    meshes_ = &meshes;
    accumulator_ = time_ = updateMs_ = 0.f;
    tick_ = 0;
    paused_ = false;
    log_.clear();
    state_.clear();
    pendingScene_.clear();
    pendingSave_.clear();
    pendingLoad_.clear();
    quit_ = false;
    begin();
    applyRequests();   // a scene may move on right away (a splash screen, say)
}

void Game::begin(bool fresh) {
    rigidBodies_.clear();
    triggerPairs_.clear();
    ctx_.spawnQueue_.clear();
    ctx_.destroyQueue_.clear();
    input_ = InputMap(scene_->inputActions);
    if (fresh)   // (a loaded save game has its animators' state already)
        for (Entity& e : scene_->entities)
            if (e.animator && e.animator->playOnStart && !e.animator->clip.empty()) {
                e.animator->playing = true;
                e.animator->time = 0.f;
                e.animator->fadeFrom.clear();
            }
    updateAnimators(*scene_, *meshes_, 0.f);   // the first frame shows the first pose
    rigidBodies_.step(*scene_, *meshes_, 0.f);   // make the bodies now, not during the first update
    physics_.build(*scene_, *meshes_);
    prepareContext(0.f);
    startAudio();
    std::vector<std::string> missing;   // behaviours whose game module isn't loaded: they do nothing
    for (const Entity& e : scene_->entities)
        for (const BehaviourPtr& b : e.behaviours)
            if (dynamic_cast<const MissingBehaviour*>(b.get()) &&
                std::find(missing.begin(), missing.end(), b->typeName()) == missing.end())
                missing.push_back(b->typeName());
    for (const std::string& type : missing) ctx_.log("Behaviour '" + type + "' has no code (game module not loaded)");
    // Index loops: behaviours may look entities up, but nothing is added or removed until flushDestroyed()
    for (size_t i = 0; i < scene_->entities.size(); ++i)
        for (size_t b = 0; b < scene_->entities[i].behaviours.size(); ++b)
            scene_->entities[i].behaviours[b]->start(ctx_, scene_->entities[i]);
    flushQueues();
}

void Game::stop() {
    stopSounds();
    rigidBodies_.clear();
    scene_ = nullptr;
}

void Game::stopSounds() {
    if (audio_)
        for (const auto& a : attached_) audio_->stop(a.second);
    attached_.clear();
}

void Game::frame(float dt, bool keyboard, bool mouse, ImVec2 mouseDelta) {
    if (!running()) return;
    input_.beginFrame(keyboard, mouse, mouseDelta);
    if (paused_) {   // nothing consumes the latched presses: drop them, so they don't all fire on resume
        input_.endFixedStep();
        return;
    }
    accumulator_ += dt;
    int steps = 0;
    while (accumulator_ >= kFixedDt && steps < kMaxStepsPerFrame) {
        fixedUpdate(kFixedDt);
        accumulator_ -= kFixedDt;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame) accumulator_ = 0.f;
    updateAudio();
}

void Game::drawUI(GameUI& ui) {
    if (!running()) return;
    for (size_t i = 0; i < scene_->entities.size(); ++i) {   // by index: a UI may request changes, not make them
        Entity& e = scene_->entities[i];
        if (e.behaviours.empty() || !scene_->visibleInHierarchy(e.id)) continue;
        for (size_t b = 0; b < e.behaviours.size(); ++b) e.behaviours[b]->drawUI(ctx_, e, ui);
    }
    // Requests made from the UI (a menu's Load, Quit) happen now, not at the next update (which may not come while
    // paused)
    applyRequests();
}

void Game::step() {
    if (running()) fixedUpdate(kFixedDt);
}

void Game::prepareContext(float dt) {
    ctx_.scene_ = scene_;
    ctx_.input_ = &input_;
    ctx_.physics_ = &physics_;
    ctx_.audio_ = audio_ && audio_->ready() ? audio_ : nullptr;
    ctx_.time_ = time_;
    ctx_.dt_ = dt;
    ctx_.log_ = &log_;
    ctx_.game_ = this;
}

void Game::fixedUpdate(float dt) {
    auto started = std::chrono::steady_clock::now();
    time_ += dt;
    ++tick_;
    updateAnimators(*scene_, *meshes_, dt);   // before physics: colliders on animated parts follow them
    physics_.build(*scene_, *meshes_);
    prepareContext(dt);
    for (size_t i = 0; i < scene_->entities.size(); ++i) {
        Entity& e = scene_->entities[i];
        if (ctx_.isDestroyed(e.id)) continue;
        for (size_t b = 0; b < e.behaviours.size(); ++b) e.behaviours[b]->update(ctx_, e);
    }
    flushQueues();
    playImpacts(rigidBodies_.step(*scene_, *meshes_, dt));
    updateTriggers();
    flushQueues();
    input_.endFixedStep();
    applyRequests();
    float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    updateMs_ = updateMs_ > 0.f ? updateMs_ * 0.9f + ms * 0.1f : ms;
}

// ---------------------------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------------------------
void Game::startAudio() {
    attached_.clear();
    if (!audio_ || !audio_->ready()) return;
    std::vector<Mat4> worlds = scene_->worldMatrices();
    for (size_t i = 0; i < scene_->entities.size(); ++i) {
        const Entity& e = scene_->entities[i];
        if (!e.audioSource || !e.audioSource->playOnStart || !scene_->visibleInHierarchy(e.id)) continue;
        const AudioSource& a = *e.audioSource;
        AudioEngine::PlayParams p;
        p.volume = a.volume;
        p.loop = a.loop;
        p.spatial = a.spatial;
        p.position = {worlds[i].m[12], worlds[i].m[13], worlds[i].m[14]};
        p.minDistance = a.minDistance;
        p.maxDistance = a.maxDistance;
        if (AudioEngine::Sound s = audio_->play(a.clip, p)) attached_.emplace_back(e.id, s);
    }
}

// Once per frame: the listener is the camera, looping sounds follow their entities, finished sounds are freed
void Game::updateAudio() {
    if (!audio_ || !audio_->ready()) return;
    CameraView cam;
    if (activeCamera(cam)) audio_->setListener(cam.position, normalize(transformDir(inverseAffine(cam.view), {0, 0, -1})));
    for (auto it = attached_.begin(); it != attached_.end();) {
        const Entity* e = scene_->find(it->first);
        if (!e || !audio_->playing(it->second)) {   // destroyed entity or finished sound
            audio_->stop(it->second);
            it = attached_.erase(it);
            continue;
        }
        audio_->setPosition(it->second, ctx_.worldPosition(e->id));
        ++it;
    }
    audio_->update();
}

void Game::playImpacts(const std::vector<Impact>& impacts) {
    if (!ctx_.audio_) return;
    for (const Impact& i : impacts) {
        const Entity* e = scene_->find(i.entity);
        if (!e || !e->audioSource || !e->audioSource->playOnImpact) continue;
        const AudioSource& a = *e->audioSource;
        AudioEngine::PlayParams p;
        p.volume = a.volume * std::min(1.f, i.speed / 8.f);
        p.spatial = a.spatial;
        p.position = i.position;
        p.minDistance = a.minDistance;
        p.maxDistance = a.maxDistance;
        ctx_.audio_->play(a.clip, p);
    }
}

// Compares this step's trigger overlaps with the previous step's and sends enter / exit to both entities
void Game::updateTriggers() {
    physics_.build(*scene_, *meshes_);   // the rigid bodies have moved
    std::vector<std::pair<EntityId, EntityId>> now = physics_.triggerOverlaps();
    auto contains = [](const std::vector<std::pair<EntityId, EntityId>>& v, const std::pair<EntityId, EntityId>& p) {
        return std::find(v.begin(), v.end(), p) != v.end();
    };
    auto notify = [&](const std::pair<EntityId, EntityId>& p, bool enter) {
        // Look the entities up again for every call: a callback may destroy them (deferred) but not reallocate
        for (int side = 0; side < 2; ++side) {
            Entity* self = scene_->find(side == 0 ? p.first : p.second);
            Entity* other = scene_->find(side == 0 ? p.second : p.first);
            if (!self || !other) return;
            for (size_t b = 0; b < self->behaviours.size(); ++b) {
                if (enter) self->behaviours[b]->onTriggerEnter(ctx_, *self, *other);
                else self->behaviours[b]->onTriggerExit(ctx_, *self, *other);
            }
        }
    };
    for (const auto& p : now)
        if (!contains(triggerPairs_, p)) notify(p, true);
    for (const auto& p : triggerPairs_)
        if (!contains(now, p)) notify(p, false);   // also when one of them was destroyed: then nobody is told
    triggerPairs_ = std::move(now);
}

// Adds spawned entities to the scene and starts their behaviours (which may spawn more: a few rounds of that)
void Game::flushSpawned() {
    for (int round = 0; round < 8 && !ctx_.spawnQueue_.empty(); ++round) {
        std::vector<Entity> batch = std::move(ctx_.spawnQueue_);
        ctx_.spawnQueue_.clear();
        std::vector<EntityId> ids;
        for (Entity& e : batch) {
            ids.push_back(e.id);
            scene_->add(std::move(e));
        }
        for (EntityId id : ids)   // by id: start() may spawn, but adding only happens in the next round
            if (Entity* e = scene_->find(id))
                for (size_t b = 0; b < e->behaviours.size(); ++b) e->behaviours[b]->start(ctx_, *e);
    }
    ctx_.spawnQueue_.clear();   // runaway spawning from start(): drop the rest
}

void Game::flushDestroyed() {
    scene_->remove(ctx_.destroyQueue_);
    ctx_.destroyQueue_.clear();
}

std::vector<Mat4> Game::worldMatrices() const {
    return scene_ ? scene_->worldMatrices() : std::vector<Mat4>{};
}

bool Game::activeCamera(CameraView& out) const {
    if (!scene_) return false;
    for (const Entity& e : scene_->entities) {
        if (!e.camera) continue;
        Mat4 world = scene_->worldMatrix(e.id);
        // Strip scale so it doesn't distort the view; the camera looks down its local -Z
        Vec3 x = normalize(transformDir(world, {1, 0, 0})), y = normalize(transformDir(world, {0, 1, 0}));
        Vec3 z = normalize(cross(x, y));
        Mat4 rigid;
        rigid.m[0] = x.x; rigid.m[1] = x.y; rigid.m[2] = x.z;
        rigid.m[4] = y.x; rigid.m[5] = y.y; rigid.m[6] = y.z;
        rigid.m[8] = z.x; rigid.m[9] = z.y; rigid.m[10] = z.z;
        rigid.m[12] = world.m[12]; rigid.m[13] = world.m[13]; rigid.m[14] = world.m[14];
        out.view = inverseAffine(rigid);
        out.position = {world.m[12], world.m[13], world.m[14]};
        out.fovY = e.camera->fovY;
        out.zNear = e.camera->zNear;
        out.zFar = e.camera->zFar;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Scene flow and save games
// ---------------------------------------------------------------------------------------------
void Game::applyRequests() {
    std::string error;
    if (!pendingSave_.empty()) {
        std::string slot = std::move(pendingSave_);
        pendingSave_.clear();
        if (saveGame(slot, error)) ctx_.log("Saved '" + slot + "'");
        else ctx_.log("Save failed: " + error);
    }
    if (!pendingLoad_.empty()) {
        std::string slot = std::move(pendingLoad_);
        pendingLoad_.clear();
        pendingScene_.clear();   // loading a save wins over a scene change asked for in the same update
        if (loadGame(slot, error)) ctx_.log("Loaded '" + slot + "'");
        else ctx_.log("Load failed: " + error);
    } else if (!pendingScene_.empty()) {
        std::string path = std::move(pendingScene_);
        pendingScene_.clear();
        if (!changeScene(path, error)) ctx_.log("loadScene: " + error);
    }
}

bool Game::changeScene(const std::string& path, std::string& error) {
    if (!running()) { error = "the game isn't running"; return false; }
    if (!meshLib_ || !texLib_) { error = "no asset libraries to load scenes into"; return false; }
    Scene next;
    if (!loadScene(project::resolve(path), next, *meshLib_, *texLib_, error)) return false;
    stopSounds();
    *scene_ = std::move(next);
    scenePath_ = path;
    begin();
    return true;
}

fs::path Game::saveGamePath(const std::string& slot) const {
    std::string file;
    for (char c : slot) file += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return (saveDir_.empty() ? project::saveDir("Untitled") : saveDir_) / (file + ".save");
}

bool Game::saveGame(const std::string& slot, std::string& error) {
    if (!running()) { error = "the game isn't running"; return false; }
    if (!meshes_ || !texLib_) { error = "no asset libraries"; return false; }
    json::Value doc = json::Value::object();
    doc.set("format", "vkworld-save");
    doc.set("version", 1);
    doc.set("scene", scenePath_);
    doc.set("time", time_);
    doc.set("tick", (double)tick_);
    doc.set("state", state_.toJson());
    doc.set("world", sceneToJson(*scene_, *meshes_, *texLib_));
    json::Value& states = doc.set("behaviours", json::Value::array());   // runtime state() of every behaviour
    for (Entity& e : scene_->entities)
        for (size_t i = 0; i < e.behaviours.size(); ++i) {
            json::Value st = behaviourStateToJson(*e.behaviours[i]);
            if (st.members().empty()) continue;
            json::Value rec = json::Value::object();
            rec.set("entity", e.id);
            rec.set("index", (int)i);
            rec.set("type", e.behaviours[i]->typeName());
            rec.set("state", std::move(st));
            states.push(std::move(rec));
        }
    json::Value& triggers = doc.set("triggers", json::Value::array());   // overlaps already reported (no re-enter)
    for (const auto& [a, b] : triggerPairs_) {
        json::Value pair = json::Value::array();
        pair.push(a);
        pair.push(b);
        triggers.push(std::move(pair));
    }

    fs::path path = saveGamePath(slot);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        if (!f) { error = "cannot write " + temp.u8string(); return false; }
        f << json::write(doc);
        if (!f) { error = "cannot write " + temp.u8string(); return false; }
    }
    fs::rename(temp, path, ec);   // a crash mid-save leaves the old save intact
    if (ec) { error = "cannot write " + path.u8string() + ": " + ec.message(); return false; }
    return true;
}

bool Game::loadGame(const std::string& slot, std::string& error) {
    if (!running()) { error = "the game isn't running"; return false; }
    if (!meshLib_ || !texLib_) { error = "no asset libraries to load scenes into"; return false; }
    fs::path path = saveGamePath(slot);
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "no save game '" + slot + "'"; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value doc;
    if (!json::parse(ss.str(), doc, error)) { error = path.u8string() + ": " + error; return false; }
    if (doc["format"].asString() != "vkworld-save") { error = path.u8string() + ": not a save game"; return false; }
    if (doc["version"].asInt(0) > 1) { error = path.u8string() + ": made by a newer version"; return false; }
    Scene next;
    if (!sceneFromJson(doc["world"], next, *meshLib_, *texLib_, error)) { error = path.u8string() + ": " + error; return false; }

    stopSounds();
    *scene_ = std::move(next);
    scenePath_ = doc["scene"].asString();
    time_ = doc["time"].asFloat(0.f);
    tick_ = (unsigned long long)doc["tick"].asNumber(0);
    accumulator_ = 0.f;
    state_.fromJson(doc["state"]);
    begin(false);   // start() everything, then put back what it had built up
    for (const json::Value& rec : doc["behaviours"].elements()) {
        Entity* e = scene_->find((EntityId)rec["entity"].asNumber(0));
        size_t i = (size_t)rec["index"].asInt(-1);
        if (!e || i >= e->behaviours.size() || rec["type"].asString() != e->behaviours[i]->typeName()) continue;
        behaviourStateFromJson(*e->behaviours[i], rec["state"]);
    }
    triggerPairs_.clear();
    for (const json::Value& pair : doc["triggers"].elements())
        if (pair.elements().size() == 2)
            triggerPairs_.emplace_back((EntityId)pair.elements()[0].asNumber(0), (EntityId)pair.elements()[1].asNumber(0));
    return true;
}
