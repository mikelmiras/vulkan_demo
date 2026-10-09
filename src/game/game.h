// Game runtime: runs the scene's behaviours with a fixed timestep, independent of the frame rate.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/vecmath.h"
#include "game/input.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

// What the game is looking through: the first entity with a Camera component.
struct CameraView {
    Mat4 view;
    Vec3 position;
    float fovY = 60.f * DEG2RAD;
    float zNear = 0.1f, zFar = 2000.f;
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

    // Removes the entity (and its children) once every behaviour has run this update, so references stay valid
    void destroy(EntityId id);
    bool isDestroyed(EntityId id) const;   // queued this update
    void log(const std::string& message);  // shown in the viewport and printed to stdout

private:
    friend class Game;
    Scene* scene_ = nullptr;
    const InputMap* input_ = nullptr;
    const PhysicsWorld* physics_ = nullptr;
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
    bool paused() const { return paused_; }
    void step();   // run exactly one fixed update (frame stepping while paused)

    float time() const { return time_; }               // simulated seconds since start()
    unsigned long long tick() const { return tick_; }  // fixed updates since start()
    InputMap& input() { return input_; }
    // Messages from GameContext::log(), with the game time they were logged at (latest last)
    const std::vector<std::pair<float, std::string>>& log() const { return log_; }

    std::vector<Mat4> worldMatrices() const;      // aligned with scene.entities
    const PhysicsWorld& physics() const { return physics_; }
    bool activeCamera(CameraView& out) const;     // the first entity with a camera; false if none

private:
    void fixedUpdate(float dt);
    void prepareContext(float dt);
    void flushDestroyed();
    void flushSpawned();
    void flushQueues() { flushSpawned(); flushDestroyed(); }
    void updateTriggers();

    Scene* scene_ = nullptr;
    const MeshLibrary* meshes_ = nullptr;
    InputMap input_;
    PhysicsWorld physics_;
    std::vector<std::pair<EntityId, EntityId>> triggerPairs_;   // overlapping after the previous update
    GameContext ctx_;
    std::vector<std::pair<float, std::string>> log_;
    float accumulator_ = 0.f, time_ = 0.f;
    unsigned long long tick_ = 0;
    bool paused_ = false;
};
