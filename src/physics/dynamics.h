// Rigid-body motion: integrates RigidBody velocities and resolves their contacts (bounce, friction, pushing other
// bodies). Linear motion only: bodies don't rotate, except that spheres roll visually.
#pragma once

#include "assets/meshes.h"
#include "scene/scene.h"

struct DynamicsSettings {
    Vec3 gravity{0, -18.f, 0};   // the same as the PlayerController's default, so things fall alike
    int maxSubsteps = 16;
};

// A body hitting something hard enough to hear
struct Impact {
    EntityId entity;
    Vec3 position;
    float speed;   // closing speed along the contact normal, m/s
};

// One fixed update for every entity with a RigidBody and a solid collider; returns the impacts (one per body at
// most, faster than 1.5 m/s)
std::vector<Impact> stepDynamics(Scene& scene, const MeshLibrary& meshes, float dt, const DynamicsSettings& settings = {});
