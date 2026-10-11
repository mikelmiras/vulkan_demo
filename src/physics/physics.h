// The physics of a scene, all on Jolt Physics: rigid bodies, collision queries, triggers and characters.
//
// - Entities with a RigidBody (and a solid collider) fall, bounce, slide, tumble and stack. Every other solid
//   collider is something they collide with: static, or kinematic once game code moves it (so moving platforms
//   push them). Trigger colliders are sensors: they report overlaps (triggerOverlaps) and block nothing.
// - Characters (entities moved with moveCharacter, with a capsule or sphere collider) are Jolt CharacterVirtuals:
//   they slide along walls, walk up slopes and steps, stick to floors, ride moving platforms and push bodies.
// - The scene stays the source of truth: each step picks up what game code changed since the previous one (moved
//   or teleported entities, velocities set by behaviours), simulates, and writes positions, rotations and
//   velocities back. Queries see the world as of the last step (plus characters moved since).
#pragma once

#include <memory>
#include <utility>
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

struct RaycastHit {
    EntityId entity = kNoEntity;
    Vec3 point, normal;
    float distance = 0.f;
};

struct CharacterMove {
    Vec3 displacement;          // how far it actually moved
    bool grounded = false;      // standing on something walkable
    bool hitCeiling = false;
    bool hitWall = false;
};

class Physics {
public:
    Physics();
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    // One fixed update; returns the impacts (new contacts closing faster than 1.5 m/s, one per body at most). With
    // dt = 0 it only brings the bodies up to date with the scene (making them all, the first time).
    std::vector<Impact> step(Scene& scene, const MeshLibrary& meshes, float dt, const DynamicsSettings& settings = {});
    void clear();                 // forget everything (the scene was replaced)
    void swap(Physics& other);    // trade worlds (one made on a loading thread, say)
    // The scene's mesh ids changed (old id -> map[old id]): mesh colliders keep their bodies
    void remapMeshes(const std::vector<int>& map);
    size_t bodyCount() const;     // bodies in the simulation (characters not included)

    // Closest solid collider along the ray (dir need not be normalised); triggers are ignored
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore = kNoEntity) const;
    // Every solid surface along the ray that faces it (one hit per convex body, per triangle for meshes), nearest
    // first: the floors stacked under a point, say
    std::vector<RaycastHit> raycastAll(Vec3 origin, Vec3 dir, float maxDistance) const;
    // The box around every body; false if there are none
    bool bounds(Vec3& min, Vec3& max) const;
    // Entities whose collider touches the sphere
    std::vector<EntityId> overlapSphere(Vec3 center, float radius, bool includeTriggers = true,
                                        EntityId ignore = kNoEntity) const;
    // (trigger, other) pairs overlapping now, triggers in scene order; triggers don't trigger each other
    std::vector<std::pair<EntityId, EntityId>> triggerOverlaps(const Scene& scene) const;

    // Moves `self` by `delta` (world space) as a character, over `dt` seconds. Needs a capsule or sphere collider
    // (without one it just moves). Standing on a moving body carries it along.
    CharacterMove moveCharacter(Scene& scene, const MeshLibrary& meshes, Entity& self, Vec3 delta, float dt);
    bool isCharacter(EntityId id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
