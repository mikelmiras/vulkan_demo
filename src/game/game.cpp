#include "game/game.h"

void Game::start(Scene& scene) {
    scene_ = &scene;
    accumulator_ = time_ = 0.f;
    tick_ = 0;
    paused_ = false;
    input_ = InputMap{};
}

void Game::stop() { scene_ = nullptr; }

void Game::frame(float dt, bool keyboard, bool mouse) {
    if (!running()) return;
    input_.beginFrame(keyboard && !paused_, mouse && !paused_);
    if (paused_) return;
    accumulator_ += dt;
    int steps = 0;
    while (accumulator_ >= kFixedDt && steps < kMaxStepsPerFrame) {
        fixedUpdate(kFixedDt);
        accumulator_ -= kFixedDt;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame) accumulator_ = 0.f;
}

void Game::step() {
    if (running()) fixedUpdate(kFixedDt);
}

void Game::fixedUpdate(float dt) {
    time_ += dt;
    ++tick_;
    input_.endFixedStep();
}

std::vector<Mat4> Game::worldMatrices() const {
    return scene_ ? scene_->worldMatrices(true, time_) : std::vector<Mat4>{};
}

bool Game::activeCamera(CameraView& out) const {
    if (!scene_) return false;
    for (size_t i = 0; i < scene_->entities.size(); ++i) {
        const Entity& e = scene_->entities[i];
        if (!e.camera) continue;
        Mat4 world = worldMatrices()[i];
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
