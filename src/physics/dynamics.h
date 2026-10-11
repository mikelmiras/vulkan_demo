// Rigid-body simulation, by Jolt Physics: entities with a RigidBody (and a solid collider) fall, bounce, slide,
// tumble and stack. Every other solid collider is something they collide with: static, or kinematic once game code
// moves it (so moving platforms and characters push them). Triggers aren't part of it.
//
// The scene stays the source of truth: each step picks up what game code changed since the previous one (moved or
// teleported entities, velocities set by behaviours), simulates, and writes positions, rotations and velocities
// back. Collision queries, characters and triggers are PhysicsWorld's (physics_world.h).
#pragma once

#include <memory>
#include <vector>

#include "assets/meshes.h"
#include "scene/scene.h"

struct DynamicsSettings {
    Vec3 gravity{0, -18.f, 0};   // the same as the PlayerController's default, so things fall alike
};

// A body hitting something hard enough to hear
struct Impact {
    EntityId entity;
    Vec3 position;
    float speed;   // closing speed along the contact normal, m/s
};

class RigidBodies {
public:
    RigidBodies();
    ~RigidBodies();
    RigidBodies(const RigidBodies&) = delete;
    RigidBodies& operator=(const RigidBodies&) = delete;

    // One fixed update; returns the impacts (new contacts closing faster than 1.5 m/s, one per body at most). With
    // dt = 0 it only brings the bodies up to date with the scene (making them all, the first time).
    std::vector<Impact> step(Scene& scene, const MeshLibrary& meshes, float dt, const DynamicsSettings& settings = {});
    void clear();                 // forget every body (the scene was replaced)
    size_t bodyCount() const;     // bodies in the simulation

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
