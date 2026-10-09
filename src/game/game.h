// Game runtime: runs the simulation over a scene with a fixed timestep, independent of the frame rate.
#pragma once

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

class Game {
public:
    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kMaxStepsPerFrame = 8;   // after a long hitch, drop time instead of spiralling

    // Takes over `scene` (the play-mode copy) until stop().
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

    // World matrices of the scene's current (animated) pose, aligned with scene.entities
    std::vector<Mat4> worldMatrices() const;
    // The first entity with a camera, or false if there is none.
    bool activeCamera(CameraView& out) const;

private:
    void fixedUpdate(float dt);

    Scene* scene_ = nullptr;
    InputMap input_;
    float accumulator_ = 0.f, time_ = 0.f;
    unsigned long long tick_ = 0;
    bool paused_ = false;
};
