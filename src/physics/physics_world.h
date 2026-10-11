// Collision queries over a scene: a snapshot of every collider in world space, rebuilt each fixed update.
// Colliders go where their entities are, characters move with moveCapsule() and slide along what they hit.
// Broad-phase: a bounding volume hierarchy over the bodies that don't move during an update (everything without
// a RigidBody), and a plain list of the rigid bodies, which stepDynamics() moves around.
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
    // After stepDynamics() has moved the rigid bodies inside this world, is it still an exact snapshot of the scene?
    // Not if a collider rides on a rigid body (a child of one), or a rolling sphere's collider is off-centre.
    bool staleAfterDynamics() const { return staleAfterDynamics_; }
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

    // Calls f(body) for every body whose bounds overlap [lo, hi] (bounds test only; f does the exact test)
    template <class F> void forEachNear(Vec3 lo, Vec3 hi, F&& f) const;
    template <class F> void forEachNear(Vec3 lo, Vec3 hi, F&& f);

    // World-space capsule of an entity's capsule collider (or a sphere's as a zero-length capsule)
    static bool capsuleOf(const Body& b, collision::Capsule& out);

    // Deepest contact of a sphere against one body; false if none
    bool contact(const collision::Sphere& s, const Body& b, collision::Contact& out) const;
    // Deepest contact of body `a` against body `b` (normal pushes `a` out); boxes and meshes as their boxes
    // against other boxes, as clusters of spheres against anything else
    bool contact(const Body& a, const Body& b, collision::Contact& out) const;

private:
    bool overlaps(const Body& a, const Body& b) const;
    void buildTree();
    int buildNode(int first, int count, int depth);
    // Indices of static bodies whose bounds overlap [lo, hi] (or the ray's path, for queryRay), into `out`
    void queryBox(Vec3 lo, Vec3 hi, std::vector<int>& out) const;
    void queryRay(Vec3 origin, Vec3 invDir, float maxT, std::vector<int>& out) const;

    struct Node {
        Vec3 lo, hi;
        int left = -1, right = -1;   // children (inner nodes)
        int first = 0, count = 0;    // range in staticIdx_ (leaves: count > 0)
    };

    const MeshLibrary* meshes_ = nullptr;
    std::vector<Body> bodies_;
    struct Prim {
        Vec3 lo, hi, center2;        // a static body's bounds, and twice its centre
    };
    std::vector<Node> nodes_;       // nodes_[0] is the root (if any)
    std::vector<Prim> prims_;       // by bodies_ index (static bodies only)
    std::vector<int> staticIdx_;    // bodies_ indices, ordered by the tree
    std::vector<int> dynamicIdx_;   // bodies_ indices of rigid bodies
    bool staleAfterDynamics_ = false;
    std::vector<int> byEntity_;     // entity id -> bodies_ index, -1 if none (ids are small and dense)
};

template <class F>
void PhysicsWorld::forEachNear(Vec3 lo, Vec3 hi, F&& f) const {
    std::vector<int> near;
    queryBox(lo, hi, near);
    for (int i : near) f(bodies_[(size_t)i]);
    for (int i : dynamicIdx_) {
        const Body& b = bodies_[(size_t)i];
        if (b.aabbMin.x <= hi.x && b.aabbMax.x >= lo.x && b.aabbMin.y <= hi.y && b.aabbMax.y >= lo.y &&
            b.aabbMin.z <= hi.z && b.aabbMax.z >= lo.z)
            f(b);
    }
}

template <class F>
void PhysicsWorld::forEachNear(Vec3 lo, Vec3 hi, F&& f) {
    static_cast<const PhysicsWorld&>(*this).forEachNear(lo, hi, [&](const Body& b) { f(const_cast<Body&>(b)); });
}
