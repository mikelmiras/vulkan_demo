#include "game/game.h"

#include "physics/physics.h"
#include "game/animation.h"
#include "game/ragdoll.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/project.h"
#include "core/vfs.h"
#include "scene/serialization.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>

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
    return physics_->moveCharacter(*scene_, *meshes_, self, delta, dt_);
}

uint32_t GameContext::layerMask(std::initializer_list<const char*> layerNames) const {
    uint32_t mask = 0;
    for (const char* n : layerNames)
        if (int l = scene_->layers.find(n); l >= 0) mask |= 1u << l;
    return mask;
}

std::vector<EntityId> GameContext::findWithTag(const std::string& tag) {
    std::vector<EntityId> out;
    for (const Entity& e : scene_->entities)
        if (e.hasTag(tag) && !isDestroyed(e.id)) out.push_back(e.id);
    for (const Entity& e : spawnQueue_)
        if (e.hasTag(tag)) out.push_back(e.id);
    return out;
}

Entity* GameContext::findFirstWithTag(const std::string& tag) {
    for (Entity& e : scene_->entities)
        if (e.hasTag(tag) && !isDestroyed(e.id)) return &e;
    for (Entity& e : spawnQueue_)
        if (e.hasTag(tag)) return &e;
    return nullptr;
}

void GameContext::send(EntityId to, const Message& message) { messages_.emplace_back(to, message); }

void GameContext::sendToTag(const std::string& tag, const Message& message) {
    for (EntityId id : findWithTag(tag)) send(id, message);
}

void GameContext::broadcast(const Message& message) {
    for (const Entity& e : scene_->entities)
        if (!e.behaviours.empty()) send(e.id, message);
    for (const Entity& e : spawnQueue_)
        if (!e.behaviours.empty()) send(e.id, message);
}

GameContext::TimerId GameContext::after(float seconds, std::function<void(GameContext&)> fn, EntityId owner) {
    Game::Timer t{game_->nextTimer_++, (double)game_->time_ + std::max(seconds, 0.f), 0.f, owner, std::move(fn)};
    game_->timers_.push_back(std::move(t));
    return game_->timers_.back().id;
}

GameContext::TimerId GameContext::every(float seconds, std::function<void(GameContext&)> fn, EntityId owner) {
    float interval = std::max(seconds, Game::kFixedDt);
    Game::Timer t{game_->nextTimer_++, (double)game_->time_ + interval, interval, owner, std::move(fn)};
    game_->timers_.push_back(std::move(t));
    return game_->timers_.back().id;
}

void GameContext::cancel(TimerId timer) {
    for (Game::Timer& t : game_->timers_)
        if (t.id == timer) t.done = true;
}

Sequence& Sequence::then(std::function<void(GameContext&)> step) {
    ctx_->after(at_, std::move(step), owner_);
    return *this;
}

void GameContext::setRagdoll(EntityId root, bool limp) { setRagdollLimp(*scene_, root, limp); }

std::string GameContext::text(const std::string& key, const std::string& fallback) const {
    return game_->strings_.get(key, fallback);
}

std::string GameContext::format(const std::string& text) const {
    return game_->state_.format(game_->strings_.translate(text));
}

void GameContext::setLanguage(const std::string& language) { game_->setLanguage(language); }
const std::string& GameContext::language() const { return game_->strings_.language(); }

void Game::setLanguage(const std::string& language) {
    if (!strings_.load(project::root(), language)) strings_.loadJson("");   // (no tables: keys show their fallbacks)
    stringsLoaded_ = true;
}

bool GameContext::loading() const { return game_->loading(); }
void GameContext::showLoadingScreen(bool show) { game_->loadingScreen_ = show; }

EntityId GameContext::streamIn(const std::string& path, Vec3 offset) {
    Game::Chunk c;
    c.root = scene_->reserveId();
    c.path = path;
    c.offset = offset;
    c.loader = std::make_unique<SceneLoader>();
    c.loader->start(project::resolve(path));
    game_->chunks_.push_back(std::move(c));
    return game_->chunks_.back().root;
}

bool GameContext::streamedIn(EntityId chunk) const { return scene_->find(chunk) != nullptr; }

void GameContext::streamOut(EntityId chunk) {
    for (Game::Chunk& c : game_->chunks_)
        if (c.root == chunk) c.cancelled = true;
    if (scene_->find(chunk)) destroy(chunk);
}

bool GameContext::findPath(Vec3 from, Vec3 to, std::vector<Vec3>& path, std::vector<uint8_t>* jumps) {
    return game_->navigation().findPath(from, to, path, jumps);
}
const NavGrid& GameContext::navigation() { return game_->navigation(); }
void GameContext::rebuildNavigation() { game_->navBaked_ = false; }

const NavGrid& Game::navigation() {
    if (navJob_.valid()) {   // the bake started with the scene: wait for it (usually long done)
        std::shared_ptr<NavGrid> baked = navJob_.get();
        nav_ = std::move(*baked);
        navBaked_ = true;
        updateNavObstacles();
    }
    if (!navBaked_ && scene_) {
        navBaked_ = true;
        if (!nav_.bake(*scene_, *meshes_, scene_->navigation)) nav_.clear();
        updateNavObstacles();
    }
    return nav_;
}

void Game::startNavigation() {
    if (navJob_.valid()) navJob_.wait();   // (an old scene's: let it finish, then drop it)
    navJob_ = {};
    nav_.clear();
    navBaked_ = false;
    bool wanted = false;   // agents or links: they'll want paths, so bake from the start
    for (const Entity& e : scene_->entities) {
        if (e.navLink) wanted = true;
        for (const BehaviourPtr& b : e.behaviours) wanted = wanted || std::string(b->typeName()) == "NavAgent";
    }
    std::string file = scenePath_.empty() ? "" : project::resolve(scenePath_) + ".nav";
    if (!wanted && (file.empty() || !vfs::exists(fs::u8path(file)))) return;   // baked when first asked for
    auto level = std::make_shared<NavLevel>(prepareNavLevel(*scene_, *meshes_));
    if (!file.empty() && nav_.load(fs::u8path(file), level->hash)) {   // a saved bake of this very level
        navBaked_ = true;
        return;
    }
    navBaked_ = true;
    navJob_ = std::async(std::launch::async, [level] {
        auto grid = std::make_shared<NavGrid>();
        if (!grid->bake(*level)) grid->clear();
        return grid;
    });
}

// Moving things block the cells they stand on (refreshed a few times a second, once there's a grid)
void Game::updateNavObstacles() {
    if (!navBaked_ || navJob_.valid() || nav_.empty()) return;
    std::vector<std::pair<Vec3, Vec3>> boxes;
    for (const Entity& e : scene_->entities) {
        if (!e.collider || e.collider->trigger) continue;
        bool moving = e.rigidBody && !e.rigidBody->kinematic;
        if (!moving && !e.hasTag("NavObstacle")) continue;
        Vec3 lo, hi;
        if (physics_.bodyBounds(e.id, lo, hi)) boxes.emplace_back(lo, hi);
    }
    nav_.setObstacles(boxes);
}

void GameContext::playSound(const std::string& clip, Vec3 position, float volume) {
    makeNoise(position, volume);   // (heard by AI brains, with or without an audio engine)
    if (!audio_) return;
    AudioEngine::PlayParams p;
    p.volume = volume;
    p.spatial = true;
    p.position = position;
    audio_->play(clip, p);
}

void GameContext::playMusic(const std::string& clip, float fadeSeconds, float volume) {
    if (audio_) audio_->playMusic(clip, fadeSeconds, volume);
}

void GameContext::playMusicLayers(const std::vector<std::string>& clips, const std::vector<float>& volumes,
                                  float fadeSeconds, float bpm, int beatsPerBar) {
    if (audio_) audio_->playMusicLayers(clips, volumes, fadeSeconds, bpm, beatsPerBar);
}

void GameContext::setMusicLayer(int layer, float volume, float fadeSeconds) {
    if (audio_) audio_->setMusicLayerVolume(layer, volume, fadeSeconds);
}

void GameContext::playStinger(const std::string& clip, const std::string& sync, float volume) {
    if (!audio_) return;
    AudioEngine::Sync s = sync == "now" ? AudioEngine::Sync::Now : sync == "bar" ? AudioEngine::Sync::Bar : AudioEngine::Sync::Beat;
    audio_->playStinger(clip, s, volume);
}

void GameContext::stopMusic(float fadeSeconds) {
    if (audio_) audio_->stopMusic(fadeSeconds);
}

void GameContext::setBusVolume(const std::string& bus, float volume) {
    if (audio_) audio_->setBusVolume(audioBusFromName(bus), volume);
}

void GameContext::duck(const std::string& bus, float volume, float seconds) {
    if (audio_) audio_->duck(audioBusFromName(bus), volume, seconds);
}

void GameContext::makeNoise(Vec3 position, float loudness, EntityId source) {
    game_->noises_.push_back({position, loudness, time_, source, ++game_->noiseSerial_});
}

json::Value* GameContext::blackboard(EntityId id) {
    Entity* e = find(id);
    return e && e->brain ? &e->brain->blackboard : nullptr;
}

void GameContext::playSound2D(const std::string& clip, float volume) {
    if (!audio_) return;
    AudioEngine::PlayParams p;
    p.volume = volume;
    audio_->play(clip, p);
}

void GameContext::emitParticles(EntityId id, int count) { game_->particles_.burst(*scene_, id, count); }

void GameContext::emitParticles(const ParticleEmitter& look, Vec3 position, int count, Vec3 up) {
    game_->particles_.emit(look, position, count, up);
}

void GameContext::destroy(EntityId id) {
    if (!isDestroyed(id)) destroyQueue_.push_back(id);
}

void GameContext::shatter(EntityId id) {
    Entity* e = find(id);
    if (!e || isDestroyed(id)) return;
    if (!e->fracture || !e->meshRenderer || e->fracture->pieceMeshes.empty() || !meshes_) {
        destroy(id);
        return;
    }
    Mat4 world = scene_->worldMatrix(id);
    Vec3 centre = transformPoint(world, {0, 0, 0});
    if (e->meshRenderer->mesh >= 0 && e->meshRenderer->mesh < meshes_->count()) {
        const Mesh& m = meshes_->get(e->meshRenderer->mesh);
        centre = transformPoint(world, (m.boundsMin + m.boundsMax) * 0.5f);
    }
    Entity whole = *e;   // (copied: spawning may move the entities)
    Vec3 velocity = whole.rigidBody ? whole.rigidBody->velocity : Vec3{};
    float mass = whole.rigidBody ? whole.rigidBody->mass : 1.f;
    const Fracture& f = *whole.fracture;
    for (size_t k = 0; k < f.pieceMeshes.size(); ++k) {
        int mesh = f.pieceMeshes[k];
        if (mesh < 0 || mesh >= meshes_->count()) continue;
        const Mesh& pm = meshes_->get(mesh);
        Entity piece;
        piece.id = scene_->reserveId();
        piece.name = whole.name + " piece " + std::to_string(k + 1);
        piece.layer = whole.layer;
        piece.tags = {"Debris"};
        piece.transform.setMatrix(world);   // (a root: where the whole was in the world)
        piece.meshRenderer = whole.meshRenderer;
        piece.meshRenderer->mesh = mesh;
        piece.collider = Collider{};
        piece.collider->shape = Collider::Shape::Mesh;   // (moving: its convex hull)
        piece.rigidBody = whole.rigidBody ? *whole.rigidBody : RigidBody{};
        piece.rigidBody->kinematic = false;
        piece.rigidBody->mass = std::max(mass / (float)f.pieceMeshes.size(), 0.01f);
        Vec3 out = transformPoint(world, (pm.boundsMin + pm.boundsMax) * 0.5f) - centre;
        piece.rigidBody->velocity = velocity + (length(out) > 1e-4f ? normalize(out) : Vec3{0, 1, 0}) * f.burst;
        if (f.pieceLife > 0.f) game_->debris_.emplace_back(piece.id, time_ + f.pieceLife);
        spawnQueue_.push_back(std::move(piece));
    }
    destroy(id);
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
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[game %7.2fs] ", time_);
    logfile::write(stamp + message);
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
    loadSettings();
    putSettings();
    pendingScene_.clear();
    pendingSave_.clear();
    pendingLoad_.clear();
    quit_ = false;
    if (!stringsLoaded_) setLanguage("en");
    begin();
    applyRequests();   // a scene may move on right away (a splash screen, say)
}

// AI brains: what each perceives, then a tick of its tree (before the behaviours: they see this update's decisions)
void Game::updateBrains() {
    PROFILE_SCOPE("AI");
    noises_.erase(std::remove_if(noises_.begin(), noises_.end(), [&](const ai::Noise& n) { return time_ - n.time > 2.f; }),
                  noises_.end());
    for (size_t i = 0; i < scene_->entities.size(); ++i) {
        Entity& e = scene_->entities[i];
        if (!e.brain || ctx_.isDestroyed(e.id) || !scene_->visibleInHierarchy(e.id) ||
            (!notSimulated_.empty() && notSimulated_.count(e.id)))
            continue;
        EntityId id = e.id;
        ai::BrainState& state = brains_[id];
        ai::perceive(ctx_, e, *e.brain, state, noises_);
        ai::tick(ctx_, e, *e.brain, state);
        if (!scene_->find(id)) brains_.erase(id);
    }
}

void Game::begin(bool fresh, bool keepPhysics) {
    if (!keepPhysics) physics_.clear();
    brains_.clear();
    noises_.clear();
    debris_.clear();
    particles_.clear();
    prevWorlds_.clear();
    prevIds_.clear();
    triggerPairs_.clear();
    ctx_.spawnQueue_.clear();
    ctx_.destroyQueue_.clear();
    input_ = InputMap(scene_->inputActions);
    if (fresh)   // (a loaded save game has its animators' state already)
        for (Entity& e : scene_->entities)
            if (e.animator && e.animator->playOnStart) {
                Animator& a = *e.animator;
                a.playing = false;
                a.state.clear();
                a.parameters.clear();
                a.layerRest.clear();
                a.rootDelta = {};
                if (!a.states.empty()) {   // a state machine starts in its start state
                    enterAnimatorState(a, a.startState.empty() ? a.states[0].name : a.startState, 0.f);
                } else if (!a.clip.empty()) {
                    a.playing = true;
                    a.time = 0.f;
                    a.fadeFrom.clear();
                }
            }
    updateAnimators(*scene_, *meshes_, 0.f);   // the first frame shows the first pose
    timers_.clear();
    ctx_.messages_.clear();
    physics_.step(*scene_, *meshes_, 0.f);   // make the bodies now (or catch up: kept ones are made already)
    startNavigation();
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
    if (settingsSaveIn_ >= 0.f) saveSettings();
    loadingScene_.clear();
    for (Chunk& c : chunks_) c.cancelled = true;
    stopSounds();
    physics_.clear();
    scene_ = nullptr;
    particles_.clear();
}

const std::vector<std::pair<std::string, json::Value>>& Game::defaultSettings() {
    static const std::vector<std::pair<std::string, json::Value>> defaults = {
        {"settings.masterVolume", json::Value(1.0)}, {"settings.musicVolume", json::Value(1.0)},
        {"settings.effectsVolume", json::Value(1.0)}, {"settings.uiVolume", json::Value(1.0)},
        {"settings.ambienceVolume", json::Value(1.0)}, {"settings.headphones", json::Value(false)}};
    return defaults;
}

void Game::loadSettings() {
    settings_.clear();
    for (const auto& [key, value] : defaultSettings()) settings_[key] = value;
    std::filesystem::path file = (saveDir_.empty() ? project::saveDir("Untitled") : saveDir_) / "settings.json";
    std::ifstream f(file, std::ios::binary);
    if (!f) return;   // (none yet: the defaults)
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), error;
    json::Value doc;
    if (!json::parse(text, doc, error)) return;
    using T = json::Value::Type;
    for (auto& [key, value] : settings_) {
        const json::Value& v = doc[key.substr(9)];   // (saved without the "settings." prefix)
        if (v.type() == T::Number && value.type() == T::Number) value = json::Value(std::clamp(v.asNumber(), 0.0, 1.0));
        else if (v.type() == T::Bool && value.type() == T::Bool) value = v;
    }
}

void Game::putSettings() {
    for (const auto& [key, value] : settings_) state_.set(key, value);
}

void Game::syncSettings(float dt) {
    for (auto& [key, value] : settings_) {
        const json::Value* now = state_.get(key);
        if (!now) {   // (erased: back to what it was)
            state_.set(key, value);
            continue;
        }
        using T = json::Value::Type;
        bool changed = value.type() == T::Bool ? now->type() == T::Bool && now->asBool() != value.asBool()
                                               : now->type() == T::Number && now->asNumber() != value.asNumber();
        if (changed) {
            value = value.type() == T::Number ? json::Value(std::clamp(now->asNumber(), 0.0, 1.0)) : *now;
            state_.set(key, value);   // (clamped)
            settingsSaveIn_ = 1.f;
        }
    }
    if (settingsSaveIn_ >= 0.f) {
        settingsSaveIn_ -= std::max(dt, 0.f);
        if (settingsSaveIn_ < 0.f) saveSettings();
    }
    if (!audio_ || !audio_->ready()) return;
    audio_->setMasterVolume(settings_["settings.masterVolume"].asFloat(1.f));
    audio_->setBusUserVolume(AudioBus::Music, settings_["settings.musicVolume"].asFloat(1.f));
    audio_->setBusUserVolume(AudioBus::Effects, settings_["settings.effectsVolume"].asFloat(1.f));
    audio_->setBusUserVolume(AudioBus::UI, settings_["settings.uiVolume"].asFloat(1.f));
    audio_->setBusUserVolume(AudioBus::Ambience, settings_["settings.ambienceVolume"].asFloat(1.f));
    audio_->setHeadphones(settings_["settings.headphones"].asBool(false));
}

void Game::saveSettings() {
    settingsSaveIn_ = -1.f;
    json::Value doc = json::Value::object();
    for (const auto& [key, value] : settings_) doc.set(key.substr(9), value);
    std::filesystem::path dir = saveDir_.empty() ? project::saveDir("Untitled") : saveDir_;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    fs::path path = dir / "settings.json", temp = dir / "settings.json.tmp";
    {
        std::ofstream f(temp, std::ios::binary);
        f << json::write(doc);
        if (!f) {
            ctx_.log("Couldn't save the settings in " + dir.u8string());
            return;
        }
    }
    fs::rename(temp, path, ec);
}

void Game::stopSounds() {
    if (audio_)
        for (const auto& a : attached_) audio_->stop(a.second);
    attached_.clear();
    soundPos_.clear();
}

void Game::frame(float dt, bool keyboard, bool mouse, ImVec2 mouseDelta) {
    if (!running()) return;
    syncSettings(dt);   // (paused too: a pause menu changes them)
    keyboard_ = keyboard;
    input_.beginFrame(keyboard && !uiController_.editingText(), mouse, mouseDelta);   // (typing isn't playing)
    pollLoads(false);
    if (loader_.busy()) {   // the next scene is loading: this one waits, frozen
        input_.endFixedStep();
        updateAudio(dt);
        return;
    }
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
    updateAudio(dt);
}

void Game::drawUI(GameUI& ui) {
    if (!running()) return;
    for (size_t i = 0; i < scene_->entities.size(); ++i) {   // by index: a UI may request changes, not make them
        Entity& e = scene_->entities[i];
        if (e.behaviours.empty() || !scene_->visibleInHierarchy(e.id)) continue;
        for (size_t b = 0; b < e.behaviours.size(); ++b) e.behaviours[b]->drawUI(ctx_, e, ui);
    }
    // The UI layouts (on top): drawn, clicked, navigated; what their buttons do happens now
    if (!loader_.busy()) {
        UILayout layout = layoutUI(*scene_, ui.width());
        UIController::Frame f;   // (also with nothing to show: the controller forgets the last menu)
        f.state = &state_;
        f.format = [this](const std::string& t) { return ctx_.format(t); };
        f.keyboard = keyboard_;
        std::vector<UIAction> actions;
        uiController_.frame(ui, *scene_, layout, f, actions);
        for (const UIAction& a : actions) runUIAction(a);
        if (!ctx_.messages_.empty()) deliverMessages();   // (now: no update may come while paused)
    }
    if (loader_.busy() && loadingScreen_) drawLoadingScreen(ui);
    // Requests made from the UI (a menu's Load, Quit) happen now, not at the next update (which may not come while
    // paused)
    applyRequests();
}

void Game::runUIAction(const UIAction& a) {
    if (a.action.empty()) return;
    if (size_t semi = a.action.find(';'); semi != std::string::npos) {   // "@hide Menu; @show Settings": each in turn
        size_t from = 0;
        while (from <= a.action.size()) {
            size_t to = a.action.find(';', from);
            if (to == std::string::npos) to = a.action.size();
            std::string one = a.action.substr(from, to - from);
            one.erase(0, one.find_first_not_of(" \t"));
            one.erase(one.find_last_not_of(" \t") + 1);
            if (!one.empty()) {
                UIAction part = a;
                part.action = one;
                runUIAction(part);
            }
            from = to + 1;
        }
        return;
    }
    if (a.action[0] != '@') {   // a message, from the element
        Message m{a.action, a.element, a.value, a.text};
        if (a.target.empty()) ctx_.broadcast(m);
        else if (Entity* t = ctx_.find(a.target)) ctx_.send(t->id, m);
        return;
    }
    size_t space = a.action.find(' ');
    std::string verb = a.action.substr(0, space), arg = space == std::string::npos ? "" : a.action.substr(space + 1);
    auto entity = [&]() { return ctx_.find(arg); };
    if (verb == "@resume") paused_ = false;
    else if (verb == "@pause") paused_ = true;
    else if (verb == "@quit") ctx_.quit();
    else if (verb == "@load") { paused_ = false; ctx_.loadScene(arg); }
    else if (verb == "@save") ctx_.saveGame(arg.empty() ? "quicksave" : arg);
    else if (verb == "@loadgame") { paused_ = false; ctx_.loadGame(arg.empty() ? "quicksave" : arg); }
    else if (verb == "@show") { if (Entity* e = entity()) e->visible = true; }
    else if (verb == "@hide") { if (Entity* e = entity()) e->visible = false; }
    else if (verb == "@toggle") { if (Entity* e = entity()) e->visible = !e->visible; }
    else ctx_.log("UI: unknown action " + a.action);
}

void Game::step() {
    if (!running()) return;
    if (loader_.busy()) finishLoading();   // (stepping a loading game: the load completes)
    else fixedUpdate(kFixedDt);
}

void Game::finishLoading() {
    if (running()) pollLoads(true);
}

void Game::pollLoads(bool wait) {
    for (size_t i = 0; i < chunks_.size();) {
        if (!wait && !chunks_[i].loader->ready()) { ++i; continue; }
        Chunk done = std::move(chunks_[i]);
        chunks_.erase(chunks_.begin() + (long)i);
        Scene chunk;
        std::string error;
        if (!meshLib_ || !texLib_) error = "no asset libraries to load scenes into";
        else if (done.loader->finish(chunk, *meshLib_, *texLib_, error) && !done.cancelled)
            addChunk(done.root, done.path, done.offset, chunk);
        if (!error.empty()) ctx_.log("streamIn: " + error);
    }
    if (loader_.busy() && (wait || loader_.ready())) completeSceneChange();
}

void Game::addChunk(EntityId rootId, const std::string& path, Vec3 offset, Scene& chunk) {
    prepareContext(ctx_.dt_);
    // Prefabs: the scene's own when it has one by that name, else added
    std::unordered_map<uint32_t, uint32_t> prefabIds;
    for (Prefab& p : chunk.prefabs) {
        const Prefab* have = scene_->findPrefab(p.name);
        prefabIds[p.id] = have ? have->id : scene_->addPrefab(p).id;
    }
    Entity root;
    root.id = rootId;
    root.name = "Chunk " + fs::u8path(path).stem().u8string();
    root.transform.position[0] = offset.x;
    root.transform.position[1] = offset.y;
    root.transform.position[2] = offset.z;
    ctx_.spawnQueue_.push_back(std::move(root));
    std::unordered_map<EntityId, EntityId> ids;
    for (const Entity& e : chunk.entities) ids[e.id] = scene_->reserveId();
    for (Entity& e : chunk.entities) {
        e.id = ids[e.id];
        e.parent = e.parent != kNoEntity && ids.count(e.parent) ? ids[e.parent] : rootId;
        if (e.prefab) {
            auto it = prefabIds.find(e.prefab->prefab);
            if (it != prefabIds.end()) e.prefab->prefab = it->second;
            else e.prefab.reset();
        }
        ctx_.spawnQueue_.push_back(std::move(e));
    }
    flushSpawned();   // added, and their behaviours start()
    navBaked_ = false;   // the level changed
}

void Game::completeSceneChange() {
    std::string path = std::move(loadingScene_), error;
    loadingScene_.clear();
    Scene next;
    if (!meshLib_ || !texLib_ || !loader_.finish(next, *meshLib_, *texLib_, error, &physics_)) {
        loadError_ = error.empty() ? std::string("no asset libraries") : error;
        ctx_.log("loadScene: " + loadError_);
        return;
    }
    loadError_.clear();
    for (Chunk& c : chunks_) c.cancelled = true;   // they were for the old scene
    stopSounds();
    *scene_ = std::move(next);
    scenePath_ = path;
    begin(true, true);   // (the loader made the physics world)
}

void Game::loadScene(const std::string& path) {
    pendingScene_ = path;
    applyRequests();
}

void Game::drawLoadingScreen(GameUI& ui) {
    float t = loader_.seconds(), show = std::fmin(t / 0.3f, 1.f);   // quick loads barely show it
    float w = ui.width(), h = ui.height(), cx = w * 0.5f, cy = h * 0.5f + 40.f;
    ui.rect(0.f, 0.f, w, h, {0.02f, 0.03f, 0.05f, 0.88f * show});
    ui.text(cx, cy - 120.f, "Loading", 56.f, {1.f, 1.f, 1.f, show}, GameUI::Align::Center);
    ui.bar(cx - 200.f, cy + 80.f, 400.f, 8.f, loader_.progress(), {1.f, 1.f, 1.f, 0.8f * show}, {1.f, 1.f, 1.f, 0.12f * show});
    for (int k = 0; k < 10; ++k) {   // a spinner
        float a = (float)k / 10.f * 2.f * PI, phase = std::fmod((float)k / 10.f - t * 1.2f + 10.f, 1.f);
        float r = 34.f, s = 12.f;
        ui.rect(cx + std::cos(a) * r - s * 0.5f, cy + std::sin(a) * r - s * 0.5f, s, s,
                {1.f, 1.f, 1.f, show * (0.15f + 0.85f * phase)}, s * 0.5f);
    }
}

void Game::prepareContext(float dt) {
    ctx_.scene_ = scene_;
    ctx_.input_ = &input_;
    ctx_.physics_ = &physics_;
    ctx_.meshes_ = meshes_;
    ctx_.audio_ = audio_ && audio_->ready() ? audio_ : nullptr;
    ctx_.time_ = time_;
    ctx_.dt_ = dt;
    ctx_.log_ = &log_;
    ctx_.game_ = this;
}

const InputMap* Game::inputFor(const Entity& e) const {
    for (const Entity* p = &e; p; p = scene_->find(p->parent)) {
        auto it = inputSources_.find(p->id);
        if (it != inputSources_.end()) return it->second;
        if (p->parent == kNoEntity) break;
    }
    return &input_;
}

EntityId Game::inputRoot(const Entity& e) const {
    for (const Entity* p = &e; p; p = scene_->find(p->parent)) {
        if (inputSources_.count(p->id)) return p->id;
        if (p->parent == kNoEntity) break;
    }
    return kNoEntity;
}

void Game::syncPhysics() {
    if (running()) physics_.step(*scene_, *meshes_, 0.f);
}

void Game::setInputSource(EntityId root, const InputMap* input) {
    if (input) inputSources_[root] = input;
    else inputSources_.erase(root);
}

void Game::setSimulated(EntityId id, bool simulated) {
    if (simulated) notSimulated_.erase(id);
    else notSimulated_.insert(id);
}

void Game::fixedUpdate(float dt) {
    PROFILE_SCOPE("Fixed update");
    if (beforeTick_) beforeTick_();
    auto started = std::chrono::steady_clock::now();
    time_ += dt;
    ++tick_;
    if (interpolate_) savePreviousWorlds();
    std::vector<FiredAnimationEvent> events;
    {
        PROFILE_SCOPE("Animation");
        updateAnimators(*scene_, *meshes_, dt, &events);
    }   // before physics: colliders on animated parts follow them
    for (size_t i = 0; i < scene_->entities.size(); ++i) {   // root motion moves the animated entity
        Entity& e = scene_->entities[i];
        if (!e.animator || !e.animator->rootMotion || length(e.animator->rootDelta) < 1e-9f) continue;
        Vec3 world = transformDir(scene_->worldMatrix(e.id), e.animator->rootDelta);
        bool character = e.collider && !e.collider->trigger &&
                         (e.collider->shape == Collider::Shape::Capsule || e.collider->shape == Collider::Shape::Sphere);
        if (character) {
            physics_.moveCharacter(*scene_, *meshes_, e, world, dt);
        } else {
            Vec3 local = e.parent != kNoEntity ? transformDir(inverseAffine(scene_->worldMatrix(e.parent)), world) : world;
            e.transform.position[0] += local.x;
            e.transform.position[1] += local.y;
            e.transform.position[2] += local.z;
        }
    }
    prepareContext(dt);
    for (const FiredAnimationEvent& ev : events)
        if (Entity* e = scene_->find(ev.entity))
            for (size_t b = 0; b < e->behaviours.size(); ++b) {
                e->behaviours[b]->onAnimationEvent(ctx_, *e, ev.name);
                e = scene_->find(ev.entity);   // (a behaviour may have spawned: the vector may have moved)
                if (!e) break;
            }
    updateBrains();
    {
        PROFILE_SCOPE("Behaviours");
        for (size_t i = 0; i < scene_->entities.size(); ++i) {
            Entity& e = scene_->entities[i];
            if (ctx_.isDestroyed(e.id) || (!notSimulated_.empty() && notSimulated_.count(e.id))) continue;
            EntityId remote = kNoEntity;
            if (!inputSources_.empty()) {   // (a remote player's entities: their input, and the world as they saw it)
                ctx_.input_ = inputFor(e);
                remote = rewindBegin_ && !e.behaviours.empty() ? inputRoot(e) : kNoEntity;
                if (remote != kNoEntity) rewindBegin_(remote);
            }
            for (size_t b = 0; b < scene_->entities[i].behaviours.size(); ++b)
                scene_->entities[i].behaviours[b]->update(ctx_, scene_->entities[i]);
            if (remote != kNoEntity && rewindEnd_) rewindEnd_();
        }
        ctx_.input_ = &input_;
        flushQueues();
        runTimers();
    }
    {
        PROFILE_SCOPE("Physics");
        std::vector<Impact> impacts = physics_.step(*scene_, *meshes_, dt);
        playImpacts(impacts);
        for (const Impact& i : impacts)   // hard enough: pre-fractured things break
            if (const Entity* e = scene_->find(i.entity); e && e->fracture && e->fracture->breakSpeed > 0.f &&
                                                          i.speed >= e->fracture->breakSpeed)
                ctx_.shatter(i.entity);
        for (Entity& e : scene_->entities)   // soft bodies: where their particles went
            if (e.softBody && physics_.softBodyPositions(e.id, e.softBody->positions)) ++e.softBody->positionsVersion;
        for (auto it = debris_.begin(); it != debris_.end();) {   // pieces whose time is up
            if (time_ >= it->second) {
                ctx_.destroy(it->first);
                it = debris_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (tick_ % 15 == 0) updateNavObstacles();
    {
        PROFILE_SCOPE("Particles");
        particles_.update(*scene_, scene_->worldMatrices(), dt);
    }
    {
        PROFILE_SCOPE("Triggers");
        updateTriggers();
    }
    flushQueues();
    input_.endFixedStep();
    if (afterTick_) afterTick_();
    applyRequests();
    float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    updateMs_ = updateMs_ > 0.f ? updateMs_ * 0.9f + ms * 0.1f : ms;
}

// ---------------------------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------------------------
void Game::startAudio() {
    attached_.clear();
    soundPos_.clear();
    haveListener_ = false;
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
        p.bus = audioBusFromName(a.bus);
        p.stream = a.stream;
        p.doppler = a.doppler;
        if (AudioEngine::Sound s = audio_->play(a.clip, p)) {
            attached_.emplace_back(e.id, s);
            soundPos_[s] = p.position;
        }
    }
}

// Once per frame: the listener is the camera, looping sounds follow their entities (with their velocities, for the
// Doppler shift), the reverb is the listener's zone's, finished sounds are freed
void Game::updateAudio(float dt) {
    if (!audio_ || !audio_->ready()) return;
    PROFILE_SCOPE("Audio");
    bool moving = dt > 1e-5f && !paused_ && !loader_.busy();
    CameraView cam;
    Vec3 listener = listener_;
    if (activeCamera(cam)) {
        listener = cam.position;
        Vec3 velocity = moving && haveListener_ ? (cam.position - listener_) * (1.f / dt) : Vec3{};
        audio_->setListener(cam.position, normalize(transformDir(inverseAffine(cam.view), {0, 0, -1})), velocity);
        listener_ = cam.position;
        haveListener_ = true;
    }
    EntityId listenerEntity = cam.entity;
    for (auto it = attached_.begin(); it != attached_.end();) {
        const Entity* e = scene_->find(it->first);
        if (!e || !audio_->playing(it->second)) {   // destroyed entity or finished sound
            audio_->stop(it->second);
            soundPos_.erase(it->second);
            it = attached_.erase(it);
            continue;
        }
        Vec3 p = ctx_.worldPosition(e->id);
        Vec3& last = soundPos_[it->second];
        audio_->setPosition(it->second, p);
        audio_->setVelocity(it->second, moving ? (p - last) * (1.f / dt) : Vec3{});
        last = p;
        ++it;
    }
    if (haveListener_) occludeSounds(listenerEntity);
    // Reverb: the zone the listener is deepest in (fading in over its blend distance at the edge)
    float bestWeight = 0.f;
    const ReverbZone* best = nullptr;
    for (const Entity& e : scene_->entities) {
        if (!e.reverbZone || !scene_->visibleInHierarchy(e.id)) continue;
        const ReverbZone& z = *e.reverbZone;
        float d = length(ctx_.worldPosition(e.id) - listener);
        float w = std::clamp((z.radius - d) / std::max(z.blend, 1e-3f), 0.f, 1.f);
        if (w > bestWeight) { bestWeight = w; best = &z; }
    }
    if (best) audio_->setReverb(best->wet * bestWeight, best->roomSize, best->damping);
    else audio_->setReverb(0.f);
    audio_->update(dt);
}

// Occlusion: a ray from the listener to each spatial sound; a solid collider in between muffles it (by its
// AudioSource's occlusion; one-shots fully). The listener's own body (the camera's nearest ancestor with a
// collider) and whatever is within half a metre of the sound (its own collider) don't count.
void Game::occludeSounds(EntityId listenerEntity) {
    EntityId body = kNoEntity;
    for (EntityId id = listenerEntity; id != kNoEntity;) {
        const Entity* e = scene_->find(id);
        if (!e) break;
        if (e->collider) { body = id; break; }
        id = e->parent;
    }
    std::unordered_map<AudioEngine::Sound, EntityId> owners;
    for (const auto& [entity, sound] : attached_) owners[sound] = entity;
    for (const AudioEngine::SpatialSound& s : audio_->spatialSounds()) {
        float strength = 1.f;
        EntityId owner = kNoEntity;
        if (auto o = owners.find(s.sound); o != owners.end()) {
            owner = o->second;
            if (const Entity* e = scene_->find(owner); e && e->audioSource) strength = e->audioSource->occlusion;
        }
        float amount = 0.f;
        Vec3 d = s.position - listener_;
        float dist = length(d);
        RaycastHit hit;
        if (strength > 0.f && dist > 0.6f && physics_.raycast(listener_, d, dist - 0.5f, hit, body) &&
            hit.entity != owner && !(owner != kNoEntity && scene_->isAncestor(owner, hit.entity)))
            amount = strength;
        audio_->setOcclusion(s.sound, amount);
    }
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
        p.bus = audioBusFromName(a.bus);
        p.doppler = a.doppler;
        ctx_.audio_->play(a.clip, p);
    }
}

// Compares this step's trigger overlaps with the previous step's and sends enter / exit to both entities
void Game::updateTriggers() {
    std::vector<std::pair<EntityId, EntityId>> now = physics_.triggerOverlaps(*scene_);
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

void Game::flushQueues() {
    // Spawned entities join first (messages may be for them), then messages go out, which may spawn and send more
    for (int round = 0; round < 8 && (!ctx_.spawnQueue_.empty() || !ctx_.messages_.empty()); ++round) {
        flushSpawned();
        deliverMessages();
    }
    flushSpawned();
    if (!ctx_.messages_.empty()) {
        ctx_.log("Messages kept sending messages: " + std::to_string(ctx_.messages_.size()) + " dropped");
        ctx_.messages_.clear();
    }
    flushDestroyed();
}

void Game::deliverMessages() {
    std::vector<std::pair<EntityId, Message>> batch = std::move(ctx_.messages_);
    ctx_.messages_.clear();
    for (const auto& [to, message] : batch) {
        Entity* e = scene_->find(to);
        if (!e || ctx_.isDestroyed(to)) continue;
        for (size_t b = 0; b < e->behaviours.size(); ++b) {
            e->behaviours[b]->onMessage(ctx_, *e, message);
            if (!(e = scene_->find(to))) break;
        }
    }
}

void Game::runTimers() {
    if (timers_.empty()) return;
    std::vector<std::pair<double, GameContext::TimerId>> due;   // (when, id): fired in that order
    for (const Timer& t : timers_)
        if (!t.done && t.at <= (double)time_ + 1e-6) due.emplace_back(t.at, t.id);
    std::sort(due.begin(), due.end());
    for (const auto& [at, id] : due) {
        // (looked up again each time: a timer's function may add or cancel timers)
        auto it = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.id == id; });
        if (it == timers_.end() || it->done) continue;
        if (it->owner != kNoEntity && (!scene_->find(it->owner) || ctx_.isDestroyed(it->owner))) {
            it->done = true;
            continue;
        }
        std::function<void(GameContext&)> fn = it->fn;   // (a copy: the vector may grow while it runs)
        if (it->interval > 0.f) it->at = std::max(it->at + it->interval, (double)time_ + 1e-4);
        else it->done = true;
        fn(ctx_);
    }
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [](const Timer& t) { return t.done; }), timers_.end());
    flushQueues();
}

void Game::flushDestroyed() {
    scene_->remove(ctx_.destroyQueue_);
    ctx_.destroyQueue_.clear();
}

void Game::savePreviousWorlds() {
    prevWorlds_ = scene_->worldMatrices();
    prevIds_.resize(scene_->entities.size());
    for (size_t i = 0; i < prevIds_.size(); ++i) prevIds_[i] = scene_->entities[i].id;
}

namespace {

// Between two world matrices: positions and scales lerped, rotations slerped
Mat4 interpolateWorld(const Mat4& a, const Mat4& b, float t) {
    if (std::memcmp(a.m, b.m, sizeof(a.m)) == 0) return b;
    auto scaleOf = [](const Mat4& m) {
        return Vec3{length(Vec3{m.m[0], m.m[1], m.m[2]}), length(Vec3{m.m[4], m.m[5], m.m[6]}),
                    length(Vec3{m.m[8], m.m[9], m.m[10]})};
    };
    auto det3 = [](const Mat4& m) {
        return dot(Vec3{m.m[0], m.m[1], m.m[2]}, cross(Vec3{m.m[4], m.m[5], m.m[6]}, Vec3{m.m[8], m.m[9], m.m[10]}));
    };
    if (det3(a) <= 0.f || det3(b) <= 0.f) return b;   // mirrored (or flat): not a rotation and scale; don't blend
    Vec3 ta{a.m[12], a.m[13], a.m[14]}, tb{b.m[12], b.m[13], b.m[14]}, sa = scaleOf(a), sb = scaleOf(b);
    return composeTRS(ta + (tb - ta) * t, slerp(rotationOf(a), rotationOf(b), t), sa + (sb - sa) * t);
}

}  // namespace

std::vector<Mat4> Game::worldMatrices() const {
    if (!scene_) return {};
    std::vector<Mat4> worlds = scene_->worldMatrices();
    if (!interpolate_ || paused_ || prevWorlds_.empty()) return worlds;
    // Drawn between the last two fixed updates, by how far the frame is into the next one: smooth motion at any
    // frame rate. Entities that didn't exist in the previous update are drawn where they are.
    float t = std::fmin(std::fmax(accumulator_ / kFixedDt, 0.f), 1.f);
    std::unordered_map<EntityId, size_t> prevIndex;
    bool sameOrder = prevIds_.size() == worlds.size();
    for (size_t i = 0; sameOrder && i < worlds.size(); ++i) sameOrder = prevIds_[i] == scene_->entities[i].id;
    if (!sameOrder)
        for (size_t i = 0; i < prevIds_.size(); ++i) prevIndex[prevIds_[i]] = i;
    for (size_t i = 0; i < worlds.size(); ++i) {
        size_t p = i;
        if (!sameOrder) {
            auto it = prevIndex.find(scene_->entities[i].id);
            if (it == prevIndex.end()) continue;
            p = it->second;
        }
        worlds[i] = interpolateWorld(prevWorlds_[p], worlds[i], t);
    }
    return worlds;
}

bool Game::activeCamera(CameraView& out) const {
    if (!scene_) return false;
    // (in a multiplayer game: the local player's own camera, if it has one)
    size_t preferred = scene_->entities.size();
    if (localPlayer_ != kNoEntity)
        for (size_t i = 0; i < scene_->entities.size(); ++i) {
            const Entity& e = scene_->entities[i];
            if (e.camera && (e.id == localPlayer_ || scene_->isAncestor(localPlayer_, e.id))) {
                preferred = i;
                break;
            }
        }
    for (size_t k = 0; k < scene_->entities.size(); ++k) {
        size_t i = preferred < scene_->entities.size() ? preferred : k;
        const Entity& e = scene_->entities[i];
        if (!e.camera) continue;
        Mat4 world = scene_->worldMatrix(e.id);
        if (interpolate_ && !paused_ && !prevWorlds_.empty()) {   // the camera moves as smoothly as what it follows
            std::vector<Mat4> worlds = worldMatrices();
            world = worlds[i];
        }
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
        out.entity = e.id;
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
        if (loader_.busy()) {      // ... and over one still loading
            Scene discarded;
            std::string ignored;
            if (meshLib_ && texLib_) loader_.finish(discarded, *meshLib_, *texLib_, ignored);
            loadingScene_.clear();
        }
        for (Chunk& c : chunks_) c.cancelled = true;
        if (loadGame(slot, error)) ctx_.log("Loaded '" + slot + "'");
        else ctx_.log("Load failed: " + error);
    } else if (!pendingScene_.empty()) {
        std::string path = std::move(pendingScene_);
        pendingScene_.clear();
        if (async_ && meshLib_ && texLib_) {
            loader_.start(project::resolve(path), true);
            loadingScene_ = path;
        } else if (!changeScene(path, error)) {
            loadError_ = error;
            ctx_.log("loadScene: " + error);
        } else {
            loadError_.clear();
        }
    }
    if (!async_ && running()) pollLoads(true);   // streaming: done within the update that asked
}

bool Game::changeScene(const std::string& path, std::string& error) {
    if (!running()) { error = "the game isn't running"; return false; }
    if (!meshLib_ || !texLib_) { error = "no asset libraries to load scenes into"; return false; }
    Scene next;
    if (!::loadScene(project::resolve(path), next, *meshLib_, *texLib_, error)) return false;
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
    putSettings();   // (the player's settings now, not the ones the save was made with)
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
