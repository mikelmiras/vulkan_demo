// Game runtime: runs the scene's behaviours with a fixed timestep, independent of the frame rate.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "audio/audio.h"
#include "core/json.h"
#include "core/vecmath.h"
#include "game/input.h"
#include "game/ui.h"
#include "physics/dynamics.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

// What the game is looking through: the first entity with a Camera component.
struct CameraView {
    Mat4 view;
    Vec3 position;
    float fovY = 60.f * DEG2RAD;
    float zNear = 0.1f, zFar = 2000.f;
};

class Game;

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

    // Collision (see physics/physics_world.h). The world is a snapshot taken at the start of the update, so it
    // doesn't see moves other behaviours make during it.
    const PhysicsWorld& physics() const { return *physics_; }
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore = kNoEntity) const {
        return physics_->raycast(origin, dir, maxDistance, hit, ignore);
    }
    std::vector<EntityId> overlapSphere(Vec3 center, float radius) const { return physics_->overlapSphere(center, radius); }
    // Moves `self` by `delta` (world space) as a character: its capsule/sphere collider slides along solid colliders.
    // Without such a collider it just moves.
    CharacterMove moveCharacter(Entity& self, Vec3 delta);

    // Creates a copy of the scene's prefab at `position` (world space), turned so its -Z faces `forward` (on top of
    // the prefab's own rotation and scale), under `parent` if given. It joins the scene after this update, when
    // its behaviours' start() runs; the returned id works with find() right away. kNoEntity if there's no prefab.
    EntityId spawn(const std::string& prefab, Vec3 position, Vec3 forward = {0, 0, -1}, EntityId parent = kNoEntity);

    // Sounds (no-ops without an audio engine): a clip name or file, at a place in the world or in stereo
    void playSound(const std::string& clip, Vec3 position, float volume = 1.f);
    void playSound2D(const std::string& clip, float volume = 1.f);

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
    // Pausing stops update() (and physics); drawUI() still runs, so a pause menu can resume
    void setPaused(bool paused);
    bool paused() const;

private:
    friend class Game;
    Game* game_ = nullptr;
    Scene* scene_ = nullptr;
    const InputMap* input_ = nullptr;
    const PhysicsWorld* physics_ = nullptr;
    AudioEngine* audio_ = nullptr;
    float time_ = 0.f, dt_ = 0.f;
    std::vector<EntityId> destroyQueue_;
    std::vector<Entity> spawnQueue_;
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
    // Once per rendered frame, after frame(): the behaviours' drawUI(), between ui.begin() and ui.end()
    void drawUI(GameUI& ui);

    float time() const { return time_; }               // simulated seconds since start()
    float updateMs() const { return updateMs_; }       // CPU time of a fixed update (smoothed), milliseconds
    unsigned long long tick() const { return tick_; }  // fixed updates since start()
    InputMap& input() { return input_; }
    // Messages from GameContext::log(), with the game time they were logged at (latest last)
    const std::vector<std::pair<float, std::string>>& log() const { return log_; }

    std::vector<Mat4> worldMatrices() const;      // aligned with scene.entities
    const PhysicsWorld& physics() const { return physics_; }
    bool activeCamera(CameraView& out) const;     // the first entity with a camera; false if none

private:
    friend class GameContext;
    void begin(bool fresh = true);   // starts the current scene: physics, sounds, animators (if fresh), start()
    void stopSounds();
    void applyRequests();   // scene changes, saves and quitting asked for during the update
    void fixedUpdate(float dt);
    void prepareContext(float dt);
    void flushDestroyed();
    void flushSpawned();
    void flushQueues() { flushSpawned(); flushDestroyed(); }
    void updateTriggers();
    void startAudio();
    void updateAudio();
    void playImpacts(const std::vector<struct Impact>& impacts);

    Scene* scene_ = nullptr;
    const MeshLibrary* meshes_ = nullptr;
    InputMap input_;
    PhysicsWorld physics_;
    RigidBodies rigidBodies_;   // (Jolt) bodies persist between updates: made again on a new scene
    std::vector<std::pair<EntityId, EntityId>> triggerPairs_;   // overlapping after the previous update
    AudioEngine* audio_ = nullptr;
    std::vector<std::pair<EntityId, AudioEngine::Sound>> attached_;   // sounds following their entity
    GameContext ctx_;
    std::vector<std::pair<float, std::string>> log_;
    float accumulator_ = 0.f, time_ = 0.f, updateMs_ = 0.f;
    unsigned long long tick_ = 0;
    bool paused_ = false;

    MeshLibrary* meshLib_ = nullptr;
    TextureLibrary* texLib_ = nullptr;
    std::string scenePath_;
    std::filesystem::path saveDir_;
    GameState state_;
    std::string pendingScene_, pendingSave_, pendingLoad_;
    bool quit_ = false;
};
