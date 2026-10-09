// Collision queries over a scene: a snapshot of every collider in world space, rebuilt each fixed update.
// Everything is kinematic (no rigid-body dynamics yet): colliders go where their entities are, characters
// move with moveCapsule() and slide along what they hit.
#pragma once

#include <utility>
#include <vector>

#include "assets/meshes.h"
#include "physics/collision.h"
#include "scene/scene.h"

struct RaycastHit {
    EntityId entity = kNoEntity;
    Vec3 point, normal;
    float distance = 0.f;
};

struct CharacterMove {
    Vec3 displacement;          // how far the capsule actually moved
    bool grounded = false;      // stood on something walkable (normal pointing up)
    bool hitCeiling = false;
    bool hitWall = false;
    // Rigid bodies it ran into, with the direction from the character into the body: GameContext pushes them
    std::vector<std::pair<EntityId, Vec3>> touched;
};

class PhysicsWorld {
public:
    struct Body {
        EntityId entity = kNoEntity;
        Collider::Shape shape = Collider::Shape::Box;
        bool trigger = false;
        collision::Obb box;           // the shape for boxes; the bounds for meshes
        collision::Sphere sphere;
        collision::Capsule capsule;
        int meshId = -1;              // mesh colliders
        const Mesh* mesh = nullptr;
        Mat4 world;
        Vec3 aabbMin, aabbMax;        // world-space bounds, for quick rejection
        bool dynamic = false;         // has a RigidBody

        void translate(Vec3 d);       // moves the shape (and its bounds)
    };

    void build(const Scene& scene, const MeshLibrary& meshes);
    const std::vector<Body>& bodies() const { return bodies_; }
    std::vector<Body>& bodies() { return bodies_; }
    const Body* find(EntityId id) const;

    // Closest solid collider along the ray (dir need not be normalised); triggers are ignored
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore = kNoEntity) const;
    // Entities whose collider touches the sphere
    std::vector<EntityId> overlapSphere(Vec3 center, float radius, bool includeTriggers = true,
                                        EntityId ignore = kNoEntity) const;
    // Moves a capsule by `delta`, pushing it out of solid colliders (sub-stepped so it can't skip through
    // thin walls). `ignore` is the moving entity itself.
    CharacterMove moveCapsule(const collision::Capsule& capsule, Vec3 delta, EntityId ignore) const;
    // (trigger, other) pairs that overlap right now; triggers don't trigger each other
    std::vector<std::pair<EntityId, EntityId>> triggerOverlaps() const;

    // World-space capsule of an entity's capsule collider (or a sphere's as a zero-length capsule)
    static bool capsuleOf(const Body& b, collision::Capsule& out);

    // Deepest contact of a sphere against one body; false if none
    bool contact(const collision::Sphere& s, const Body& b, collision::Contact& out) const;
    // Deepest contact of body `a` against body `b` (normal pushes `a` out); boxes and meshes as their boxes
    // against other boxes, as clusters of spheres against anything else
    bool contact(const Body& a, const Body& b, collision::Contact& out) const;

private:
    bool overlaps(const Body& a, const Body& b) const;

    const MeshLibrary* meshes_ = nullptr;
    std::vector<Body> bodies_;
};
