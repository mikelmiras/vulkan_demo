// Game runtime: runs the scene's behaviours with a fixed timestep, independent of the frame rate.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/vecmath.h"
#include "game/input.h"
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

    Entity* find(EntityId id) { return scene_->find(id); }
    Entity* find(const std::string& name) { return scene_->findByName(name); }
    Vec3 worldPosition(EntityId id) const;

    // Removes the entity (and its children) once every behaviour has run this update, so references stay valid
    void destroy(EntityId id);
    bool isDestroyed(EntityId id) const;   // queued this update
    void log(const std::string& message);  // shown in the viewport and printed to stdout

private:
    friend class Game;
    Scene* scene_ = nullptr;
    const InputMap* input_ = nullptr;
    float time_ = 0.f, dt_ = 0.f;
    std::vector<EntityId> destroyQueue_;
    std::vector<std::pair<float, std::string>>* log_ = nullptr;
};

class Game {
public:
    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kMaxStepsPerFrame = 8;   // after a long hitch, drop time instead of spiralling

    // Takes over `scene` (the play-mode copy) until stop(), and calls every behaviour's start().
    void start(Scene& scene);
    void stop();
    bool running() const { return scene_ != nullptr; }

    // Once per rendered frame: latches input, then runs as many fixed updates as the elapsed time calls for.
    // keyboard/mouse = false while the UI owns that device.
    void frame(float dt, bool keyboard, bool mouse);
    void setPaused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    void step();   // run exactly one fixed update (frame stepping while paused)

    float time() const { return time_; }               // simulated seconds since start()
    unsigned long long tick() const { return tick_; }  // fixed updates since start()
    InputMap& input() { return input_; }
    // Messages from GameContext::log(), with the game time they were logged at (latest last)
    const std::vector<std::pair<float, std::string>>& log() const { return log_; }

    std::vector<Mat4> worldMatrices() const;      // aligned with scene.entities
    bool activeCamera(CameraView& out) const;     // the first entity with a camera; false if none

private:
    void fixedUpdate(float dt);
    void prepareContext(float dt);
    void flushDestroyed();

    Scene* scene_ = nullptr;
    InputMap input_;
    GameContext ctx_;
    std::vector<std::pair<float, std::string>> log_;
    float accumulator_ = 0.f, time_ = 0.f;
    unsigned long long tick_ = 0;
    bool paused_ = false;
};
