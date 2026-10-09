#include "game/game.h"

#include <algorithm>
#include <cstdio>

// ---------------------------------------------------------------------------------------------
// GameContext
// ---------------------------------------------------------------------------------------------
Vec3 GameContext::worldPosition(EntityId id) const {
    Mat4 m = scene_->worldMatrix(id);
    return {m.m[12], m.m[13], m.m[14]};
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
    }
    // World-space displacement -> the entity's parent space
    Vec3 local = transformDir(inverseAffine(scene_->parentWorldMatrix(self.id)), result.displacement);
    self.transform.position[0] += local.x;
    self.transform.position[1] += local.y;
    self.transform.position[2] += local.z;
    return result;
}

void GameContext::destroy(EntityId id) {
    if (!isDestroyed(id)) destroyQueue_.push_back(id);
}

bool GameContext::isDestroyed(EntityId id) const {
    return std::find(destroyQueue_.begin(), destroyQueue_.end(), id) != destroyQueue_.end();
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
    triggerPairs_.clear();
    accumulator_ = time_ = 0.f;
    tick_ = 0;
    paused_ = false;
    input_ = InputMap{};
    log_.clear();

    physics_.build(*scene_, *meshes_);
    prepareContext(0.f);
    // Index loops: behaviours may look entities up, but nothing is added or removed until flushDestroyed()
    for (size_t i = 0; i < scene_->entities.size(); ++i)
        for (size_t b = 0; b < scene_->entities[i].behaviours.size(); ++b)
            scene_->entities[i].behaviours[b]->start(ctx_, scene_->entities[i]);
    flushDestroyed();
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

void Game::prepareContext(float dt) {
    ctx_.scene_ = scene_;
    ctx_.input_ = &input_;
    ctx_.physics_ = &physics_;
    ctx_.time_ = time_;
    ctx_.dt_ = dt;
    ctx_.log_ = &log_;
}

void Game::fixedUpdate(float dt) {
    time_ += dt;
    ++tick_;
    physics_.build(*scene_, *meshes_);
    prepareContext(dt);
    for (size_t i = 0; i < scene_->entities.size(); ++i) {
        Entity& e = scene_->entities[i];
        if (ctx_.isDestroyed(e.id)) continue;
        for (size_t b = 0; b < e.behaviours.size(); ++b) e.behaviours[b]->update(ctx_, e);
    }
    flushDestroyed();
    updateTriggers();
    flushDestroyed();
    input_.endFixedStep();
}

// Compares this step's trigger overlaps with the previous step's and sends enter / exit to both entities
void Game::updateTriggers() {
    physics_.build(*scene_, *meshes_);   // everything has moved during the update
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

void Game::flushDestroyed() {
    for (EntityId id : ctx_.destroyQueue_) scene_->remove(id);
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
