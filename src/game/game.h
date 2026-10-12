// Game runtime: runs the scene's behaviours with a fixed timestep, independent of the frame rate.
#pragma once

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <initializer_list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>

#include "audio/audio.h"
#include "core/json.h"
#include "core/vecmath.h"
#include "game/input.h"
#include "game/navigation.h"
#include "game/scene_loader.h"
#include "game/strings.h"
#include "game/particles.h"
#include "game/ui.h"
#include "game/ui_layout.h"
#include "physics/physics.h"
#include "scene/scene.h"

// What the game is looking through: the first entity with a Camera component.
struct CameraView {
    Mat4 view;
    Vec3 position;
    float fovY = 60.f * DEG2RAD;
    float zNear = 0.1f, zFar = 2000.f;
};

class Game;
class GameContext;

// A chain of timed steps: ctx.sequence(self.id).wait(1.f).then(openDoor).wait(2.f).then(closeDoor). Each wait()
// adds to the time from now; then() runs at the time reached so far. Dropped with its owner entity.
class Sequence {
public:
    Sequence& wait(float seconds) {
        at_ += seconds;
        return *this;
    }
    Sequence& then(std::function<void(GameContext&)> step);

private:
    friend class GameContext;
    Sequence(GameContext& ctx, uint32_t owner) : ctx_(&ctx), owner_(owner) {}
    GameContext* ctx_;
    uint32_t owner_;
    float at_ = 0.f;
};

// Values that outlive a scene (score, lives, inventory, progress): kept when ctx.loadScene() changes the scene,
// saved in save games, empty when the game starts
class GameState {
public:
    float number(const std::string& key, float fallback = 0.f) const;
    std::string text(const std::string& key, const std::string& fallback = "") const;
    bool flag(const std::string& key, bool fallback = false) const;
    void setNumber(const std::string& key, float value) { values_[key] = value; }
    void setText(const std::string& key, const std::string& value) { values_[key] = value; }
    void setFlag(const std::string& key, bool value) { values_[key] = value; }
    float add(const std::string& key, float amount) {   // number += amount; returns the new value
        setNumber(key, number(key) + amount);
        return number(key);
    }
    bool has(const std::string& key) const { return values_.count(key) > 0; }
    const json::Value* get(const std::string& key) const {   // the value whatever its type (nullptr: none)
        auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }
    // "Score: {score}" -> "Score: 12": {key} becomes the value (whole numbers without decimals; unknown keys: "")
    std::string format(const std::string& text) const;
    void erase(const std::string& key) { values_.erase(key); }
    void clear() { values_.clear(); }
    json::Value toJson() const;
    void fromJson(const json::Value& v);

private:
    std::map<std::string, json::Value> values_;
};

// Everything a behaviour can reach while it runs
class GameContext {
public:
    Scene& scene() { return *scene_; }
    const InputMap& input() const { return *input_; }
    float time() const { return time_; }   // seconds since Play
    float dt() const { return dt_; }       // length of this update (Game::kFixedDt)

    // Also finds entities spawned during this update; a pointer to one of those lasts until the next spawn()
    Entity* find(EntityId id);
    Entity* find(const std::string& name);
    Vec3 worldPosition(EntityId id) const;

    // Collision (see physics/physics.h). Queries see the world as of the last physics step (plus characters moved
    // since), not moves other behaviours make during this update.
    const Physics& physics() const { return *physics_; }
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore = kNoEntity) const {
        return physics_->raycast(origin, dir, maxDistance, hit, ignore);
    }
    std::vector<EntityId> overlapSphere(Vec3 center, float radius) const { return physics_->overlapSphere(center, radius); }
    // The same, seeing only some layers: ctx.layerMask({"Enemy", "Default"}) (unknown names add nothing)
    uint32_t layerMask(std::initializer_list<const char*> layerNames) const;
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore, uint32_t layers) const {
        return physics_->raycast(origin, dir, maxDistance, hit, ignore, layers);
    }
    std::vector<EntityId> overlapSphere(Vec3 center, float radius, uint32_t layers) const {
        return physics_->overlapSphere(center, radius, true, kNoEntity, layers);
    }

    // Tags (Entity::tags): every entity with one, in scene order (spawned ones included), or the first
    std::vector<EntityId> findWithTag(const std::string& tag);
    Entity* findFirstWithTag(const std::string& tag);

    // Messages: delivered to the entity's behaviours (onMessage) once this update's behaviours have run (or the
    // trigger callbacks, if sent from those). Messages sent while delivering go out in the same round, up to a limit.
    void send(EntityId to, const Message& message);
    void sendToTag(const std::string& tag, const Message& message);
    void broadcast(const Message& message);   // to every entity with behaviours

    // Timers, in game time (paused with the game), run after the update they come due in. With an owner they're
    // dropped when it's destroyed. Not kept in save games or across scene changes.
    using TimerId = uint32_t;
    TimerId after(float seconds, std::function<void(GameContext&)> fn, EntityId owner = kNoEntity);
    TimerId every(float seconds, std::function<void(GameContext&)> fn, EntityId owner = kNoEntity);
    void cancel(TimerId timer);
    Sequence sequence(EntityId owner = kNoEntity) { return Sequence(*this, owner); }
    // Moves `self` by `delta` (world space) as a character: with a capsule or sphere collider it slides along walls,
    // climbs slopes and steps, rides moving platforms and pushes rigid bodies. Without one it just moves.
    CharacterMove moveCharacter(Entity& self, Vec3 delta);

    // A ragdoll (game/ragdoll.h) under `root` goes limp (simulated, its Animator stopped) or back to animation
    void setRagdoll(EntityId root, bool limp);

    // Navigation (see game/navigation.h): a walking path around walls and over steps, as corner points on the
    // ground from `from` to (nearest reachable spot to) `to`; false if there's no way. The walkable grid comes
    // from a saved bake next to the scene file (<scene>.vkw.nav, the editor's "Bake and save") when it matches the
    // level, else it's baked: on a worker thread from the start when the scene has NavAgents or NavLinks, or the
    // first time it's needed. Rigid bodies (and entities tagged "NavObstacle") block the cells under them.
    // `jumps`, when given, marks the off-mesh links along it (see NavGrid::findPath).
    bool findPath(Vec3 from, Vec3 to, std::vector<Vec3>& path, std::vector<uint8_t>* jumps = nullptr);
    const NavGrid& navigation();
    void rebuildNavigation();   // after the level itself changed (static colliders added, moved or removed)

    // Creates a copy of the scene's prefab at `position` (world space), turned so its -Z faces `forward` (on top of
    // the prefab's own rotation and scale), under `parent` if given. It joins the scene after this update, when
    // its behaviours' start() runs; the returned id works with find() right away. kNoEntity if there's no prefab.
    EntityId spawn(const std::string& prefab, Vec3 position, Vec3 forward = {0, 0, -1}, EntityId parent = kNoEntity);

    // Sounds (no-ops without an audio engine): a clip name or file, at a place in the world or in stereo
    void playSound(const std::string& clip, Vec3 position, float volume = 1.f);
    void playSound2D(const std::string& clip, float volume = 1.f);
    // Music: one track, streamed and looping on the music bus, cross-fading from the one before
    void playMusic(const std::string& clip, float fadeSeconds = 1.f, float volume = 1.f);
    void stopMusic(float fadeSeconds = 1.f);
    // The mixer's buses ("effects", "music", "ui", "ambience"): a volume, or lowered for a while (under a voice line)
    void setBusVolume(const std::string& bus, float volume);

    // Text in the player's language (game/strings.h): a key's text (or `fallback`), text with {@key}s and
    // {state values} filled in, and the language (loads strings/<language>.json)
    std::string text(const std::string& key, const std::string& fallback = "") const;
    std::string format(const std::string& text) const;
    void setLanguage(const std::string& language);
    const std::string& language() const;
    void duck(const std::string& bus, float volume, float seconds);

    // Particles: a burst from the entity's ParticleEmitter (`count`, or its burst size), or any look anywhere
    void emitParticles(EntityId id, int count = -1);
    void emitParticles(const ParticleEmitter& look, Vec3 position, int count, Vec3 up = {0, 1, 0});

    // Removes the entity (and its children) once every behaviour has run this update, so references stay valid
    void destroy(EntityId id);
    bool isDestroyed(EntityId id) const;   // queued this update
    void log(const std::string& message);  // shown in the viewport and printed to stdout

    // ---- Scene flow. These take effect at the end of the update (after every behaviour has run).
    // Replaces the scene with another scene file (relative to the project). state() and the clock carry on.
    void loadScene(const std::string& path);
    void quit();                                       // vkgame exits; the editor stops playing
    GameState& state();                                // what survives scene changes
    // Save games: the scene as it is now (entities, behaviours' fields and state(), rigid-body velocities), state()
    // and the clock, in the project's save folder (project::saveDir). Loading replaces the scene with the saved one.
    void saveGame(const std::string& slot = "quicksave");
    void loadGame(const std::string& slot = "quicksave");
    bool hasSaveGame(const std::string& slot = "quicksave") const;
    const std::string& scenePath() const;              // the current scene's file ("" for a scene built in code)
    // Loading: with asynchronous loading on (Game::setAsyncLoading, as vkgame and the editor do), loadScene()
    // reads the next scene in the background. The current one stays on screen, frozen (no updates; drawUI() still
    // runs, so a behaviour can draw its own loading screen while loading() is true), under a default loading screen
    // unless showLoadingScreen(false).
    bool loading() const;
    void showLoadingScreen(bool show);

    // Streaming: adds another scene file's entities to this scene, under a new root entity placed at `offset`
    // (loaded in the background with asynchronous loading, else at the end of this update). Their behaviours
    // start() as they arrive. Returns that root's id: streamedIn() tells when it's there, streamOut() removes it all
    // (or cancels the load). The file's prefabs join the scene's (by name); its settings are ignored.
    EntityId streamIn(const std::string& path, Vec3 offset = {});
    bool streamedIn(EntityId chunk) const;
    void streamOut(EntityId chunk);

    // Pausing stops update() (and physics); drawUI() still runs, so a pause menu can resume
    void setPaused(bool paused);
    bool paused() const;

private:
    friend class Game;
    Game* game_ = nullptr;
    Scene* scene_ = nullptr;
    const InputMap* input_ = nullptr;
    Physics* physics_ = nullptr;
    const MeshLibrary* meshes_ = nullptr;
    AudioEngine* audio_ = nullptr;
    float time_ = 0.f, dt_ = 0.f;
    std::vector<EntityId> destroyQueue_;
    std::vector<Entity> spawnQueue_;
    std::vector<std::pair<EntityId, Message>> messages_;
    std::vector<std::pair<float, std::string>>* log_ = nullptr;
};

class Game {
public:
    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kMaxStepsPerFrame = 8;   // after a long hitch, drop time instead of spiralling

    // Takes over `scene` (the play-mode copy) until stop(), and calls every behaviour's start().
    // `meshes` is needed for mesh colliders.
    void start(Scene& scene, const MeshLibrary& meshes);
    void stop();
    bool running() const { return scene_ != nullptr; }

    // Once per rendered frame: latches input, then runs as many fixed updates as the elapsed time calls for.
    // keyboard/mouse = false while the UI owns that device. mouseDelta: this frame's mouse movement (raw while
    // the cursor is captured); the overload without it uses Dear ImGui's.
    void frame(float dt, bool keyboard, bool mouse, ImVec2 mouseDelta);
    void frame(float dt, bool keyboard, bool mouse) { frame(dt, keyboard, mouse, ImGui::GetIO().MouseDelta); }
    void setPaused(bool paused) { paused_ = paused; }
    // Optional: with an audio engine, AudioSources play and ctx.playSound() is heard. Set before start().
    void setAudio(AudioEngine* audio) { audio_ = audio; }
    // For ctx.loadScene() / loadGame(): the libraries scenes load into (imports are appended). Without them, scene
    // changes fail (logged). The meshes must be the ones given to start().
    void setAssetLibraries(MeshLibrary& meshes, TextureLibrary& textures) { meshLib_ = &meshes; texLib_ = &textures; }
    // Scene changes and streaming load on a worker thread (off by default: everything happens within the update
    // that asked, which keeps tests and tools deterministic). Needs setAssetLibraries().
    void setAsyncLoading(bool on) { async_ = on; }
    // The language the game starts in (strings/<language>.json in the project); ctx.setLanguage changes it
    void setLanguage(const std::string& language);
    const StringTable& strings() const { return strings_; }
    GameContext& context() { return ctx_; }   // what behaviours get (for hosts and tests)
    bool loading() const { return loader_.busy(); }
    void finishLoading();   // waits for loads in progress and applies them
    float loadingProgress() const { return loader_.progress(); }
    const std::string& loadError() const { return loadError_; }   // why the last scene change failed ("" if it didn't)
    // Changes scene as ctx.loadScene() does, from outside the game (vkgame starts its first scene this way, so it
    // loads behind the loading screen too)
    void loadScene(const std::string& path);
    void setScenePath(const std::string& path) { scenePath_ = path; }   // the scene's file, recorded in save games
    const std::string& scenePath() const { return scenePath_; }
    void setSaveDirectory(const std::filesystem::path& dir) { saveDir_ = dir; }
    const std::filesystem::path& saveDirectory() const { return saveDir_; }
    bool quitRequested() const { return quit_; }
    GameState& state() { return state_; }

    // The same as the GameContext requests, done right away (between updates)
    bool changeScene(const std::string& path, std::string& error);
    bool saveGame(const std::string& slot, std::string& error);
    bool loadGame(const std::string& slot, std::string& error);
    std::filesystem::path saveGamePath(const std::string& slot) const;
    bool paused() const { return paused_; }
    void step();   // run exactly one fixed update (frame stepping while paused)
    // Once per rendered frame, after frame(): the behaviours' drawUI(), then the scene's UI layouts (UIElement
    // components, game/ui_layout.h), between ui.begin() and ui.end()
    void drawUI(GameUI& ui);
    const UIController& uiController() const { return uiController_; }

    // ---- Multiplayer (net/net_session.h)
    // The behaviours of `root` and everything under it read `input` (a remote player's) instead of this machine's;
    // null: back to this machine's
    void setInputSource(EntityId root, const InputMap* input);
    // Not simulated here: its behaviours don't run (another machine runs it and sends where it is)
    void setSimulated(EntityId id, bool simulated);
    bool simulated(EntityId id) const { return !notSimulated_.count(id); }
    // This machine's player: the game looks through a camera under it (rather than the first one in the scene)
    void setLocalPlayer(EntityId id) { localPlayer_ = id; }
    EntityId localPlayer() const { return localPlayer_; }
    // Called just before and after every fixed update (feeding remote inputs, sending this machine's)
    void setTickHooks(std::function<void()> before, std::function<void()> after) {
        beforeTick_ = std::move(before);
        afterTick_ = std::move(after);
    }
    Scene* scene() { return scene_; }
    // Entities joining the scene at the next update, their behaviours started then (like spawns; their ids kept)
    void addEntities(std::vector<Entity> entities) {
        for (Entity& e : entities) ctx_.spawnQueue_.push_back(std::move(e));
    }

    float time() const { return time_; }               // simulated seconds since start()
    float updateMs() const { return updateMs_; }       // CPU time of a fixed update (smoothed), milliseconds
    unsigned long long tick() const { return tick_; }  // fixed updates since start()
    InputMap& input() { return input_; }
    // Messages from GameContext::log(), with the game time they were logged at (latest last)
    const std::vector<std::pair<float, std::string>>& log() const { return log_; }

    std::vector<Mat4> worldMatrices() const;      // aligned with scene.entities
    const NavGrid& navigation();                  // baked on first use (see GameContext::findPath)
    const Physics& physics() const { return physics_; }
    void setRecordContacts(bool on) { physics_.setRecordContacts(on); }   // (Physics::contacts(): for showing them)
    const ParticleSystem& particles() const { return particles_; }
    bool activeCamera(CameraView& out) const;     // the first entity with a camera; false if none
    // Render interpolation (on by default): worldMatrices() and activeCamera() blend the last two fixed updates
    // by how far the frame is into the next one, so motion is smooth when frames and updates don't line up
    void setInterpolation(bool on) { interpolate_ = on; }
    bool interpolation() const { return interpolate_; }

private:
    friend class GameContext;
    void begin(bool fresh = true, bool keepPhysics = false);   // starts the current scene: physics, sounds, animators (if fresh), start()
    void stopSounds();
    void applyRequests();   // scene changes, saves and quitting asked for during the update
    void fixedUpdate(float dt);
    void prepareContext(float dt);
    void flushDestroyed();
    void flushSpawned();
    void flushQueues();
    void deliverMessages();   // one round
    void runTimers();
    void updateTriggers();
    void startAudio();
    void updateAudio(float dt);
    void playImpacts(const std::vector<struct Impact>& impacts);
    void savePreviousWorlds();
    void pollLoads(bool wait);   // applies finished loads (all of them, waiting, if `wait`)
    void completeSceneChange();
    void addChunk(EntityId root, const std::string& path, Vec3 offset, Scene& chunk);
    void drawLoadingScreen(GameUI& ui);
    void runUIAction(const UIAction& a);   // a button pressed in a UI layout (game/ui_layout.h)
    UIController uiController_;
    std::unordered_map<EntityId, const InputMap*> inputSources_;
    std::unordered_set<EntityId> notSimulated_;
    EntityId localPlayer_ = kNoEntity;
    std::function<void()> beforeTick_, afterTick_;
    const InputMap* inputFor(const Entity& e) const;
    bool keyboard_ = true;   // the last frame's: whether the UI may read keys

    Scene* scene_ = nullptr;
    const MeshLibrary* meshes_ = nullptr;
    InputMap input_;
    Physics physics_;   // (Jolt) bodies persist between updates: made again on a new scene
    ParticleSystem particles_;
    StringTable strings_;
    bool stringsLoaded_ = false;
    NavGrid nav_;
    bool navBaked_ = false;                            // nav_ is up to date (or navJob_ will make it so)
    std::future<std::shared_ptr<NavGrid>> navJob_;     // a bake on a worker thread, started with the scene
    void startNavigation();
    void updateNavObstacles();
    bool async_ = false, loadingScreen_ = true;
    SceneLoader loader_;   // the next scene, when changing scenes asynchronously
    std::string loadingScene_, loadError_;
    struct Chunk {   // a scene being streamed in
        EntityId root = kNoEntity;
        std::string path;
        Vec3 offset;
        std::unique_ptr<SceneLoader> loader;
        bool cancelled = false;
    };
    std::vector<Chunk> chunks_;
    struct Timer {
        GameContext::TimerId id;
        double at;
        float interval;   // 0: once
        EntityId owner;
        std::function<void(GameContext&)> fn;
        bool done = false;
    };
    std::vector<Timer> timers_;
    GameContext::TimerId nextTimer_ = 1;
    std::vector<std::pair<EntityId, EntityId>> triggerPairs_;   // overlapping after the previous update
    AudioEngine* audio_ = nullptr;
    std::vector<std::pair<EntityId, AudioEngine::Sound>> attached_;   // sounds following their entity
    std::unordered_map<AudioEngine::Sound, Vec3> soundPos_;           // ... where they were last frame
    Vec3 listener_;
    bool haveListener_ = false;
    GameContext ctx_;
    std::vector<std::pair<float, std::string>> log_;
    float accumulator_ = 0.f, time_ = 0.f, updateMs_ = 0.f;
    unsigned long long tick_ = 0;
    bool paused_ = false;
    bool interpolate_ = true;
    std::vector<Mat4> prevWorlds_;   // world matrices before the last fixed update (render interpolation)
    std::vector<EntityId> prevIds_;

    MeshLibrary* meshLib_ = nullptr;
    TextureLibrary* texLib_ = nullptr;
    std::string scenePath_;
    std::filesystem::path saveDir_;
    GameState state_;
    std::string pendingScene_, pendingSave_, pendingLoad_;
    bool quit_ = false;
};
