#include "physics/dynamics.h"

#include <cmath>

#include "physics/physics_world.h"

using namespace collision;

namespace {

// Bounce and friction for a body moving with velocity `v` into a surface with normal `n` (pointing at the body).
// `other` is the velocity of what it hit (zero for static colliders).
Vec3 respond(Vec3 v, Vec3 n, Vec3 other, const RigidBody& rb) {
    Vec3 rel = v - other;
    float vn = dot(rel, n);
    if (vn >= 0.f) return v;   // already separating
    float e = vn < -1.f ? rb.bounciness : 0.f;   // slow contacts don't bounce: things come to rest
    Vec3 tangent = rel - n * vn;
    float tl = length(tangent), drop = rb.friction * -vn * (1.f + e);
    tangent = tl <= drop ? Vec3{} : tangent * ((tl - drop) / tl);
    return other + tangent + n * (-vn * e);
}

}  // namespace

std::vector<Impact> stepDynamics(Scene& scene, const MeshLibrary& meshes, float dt, const DynamicsSettings& settings) {
    std::vector<Impact> impacts;
    PhysicsWorld world;
    world.build(scene, meshes);
    std::vector<PhysicsWorld::Body>& bodies = world.bodies();

    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        if (!bodies[bi].dynamic) continue;
        Entity* e = scene.find(bodies[bi].entity);
        RigidBody& rb = *e->rigidBody;
        if (rb.useGravity) rb.velocity += settings.gravity * dt;
        rb.velocity = rb.velocity * std::max(0.f, 1.f - rb.damping * dt);

        // Sub-steps no longer than half the body's smallest size: fast bodies can't skip through thin walls
        PhysicsWorld::Body& self = bodies[bi];
        float size = self.shape == Collider::Shape::Sphere    ? self.sphere.radius
                     : self.shape == Collider::Shape::Capsule ? self.capsule.radius
                                                              : std::min({self.box.half.x, self.box.half.y, self.box.half.z});
        Vec3 start = self.box.center;
        float hardest = 0.f;   // fastest closing speed of this update's contacts
        Vec3 groundNormal;
        bool grounded = false;
        float remaining = dt;
        int steps = std::clamp((int)std::ceil(length(rb.velocity) * dt / std::max(size * 0.5f, 1e-3f)), 1, settings.maxSubsteps);
        for (int s = 0; s < steps; ++s) {
            float h = remaining / (float)(steps - s);
            remaining -= h;
            self.translate(rb.velocity * h);
            for (int iter = 0; iter < 4; ++iter) {
                Contact deepest;
                size_t hit = bodies.size();
                for (size_t oi = 0; oi < bodies.size(); ++oi) {
                    const PhysicsWorld::Body& o = bodies[oi];
                    if (oi == bi || o.trigger || o.entity == self.entity) continue;
                    Contact c;
                    if (world.contact(self, o, c) && c.depth > deepest.depth) {
                        deepest = c;
                        hit = oi;
                    }
                }
                if (hit == bodies.size()) break;
                PhysicsWorld::Body& other = bodies[hit];
                Vec3 n = deepest.normal;
                if (n.y > 0.7f) { grounded = true; groundNormal = n; }
                hardest = std::max(hardest, -dot(rb.velocity, n));
                Entity* oe = other.dynamic ? scene.find(other.entity) : nullptr;
                if (oe && oe->rigidBody) {
                    // Two bodies: split the separation and exchange momentum by their masses
                    RigidBody& orb = *oe->rigidBody;
                    float wa = 1.f / rb.mass, wb = 1.f / orb.mass, w = wa + wb;
                    self.translate(n * (deepest.depth * wa / w));
                    other.translate(n * (-deepest.depth * wb / w));
                    float vn = dot(rb.velocity - orb.velocity, n);
                    if (vn < 0.f) {
                        float e = vn < -1.f ? std::min(rb.bounciness, orb.bounciness) : 0.f;
                        float j = -(1.f + e) * vn / w;
                        rb.velocity += n * (j * wa);
                        orb.velocity += n * (-j * wb);
                    }
                } else {
                    self.translate(n * (deepest.depth + 0.0005f));
                    rb.velocity = respond(rb.velocity, n, {}, rb);
                }
            }
        }
        if (grounded && length(rb.velocity) < 0.05f) rb.velocity = {};   // at rest
        if (hardest > 1.5f) impacts.push_back({self.entity, self.box.center, hardest});

        // Write the motion back to the entity (and the other bodies it shoved, through their own update)
        Vec3 moved = self.box.center - start;
        Vec3 local = transformDir(inverseAffine(scene.parentWorldMatrix(e->id)), moved);
        Transform& t = e->transform;
        t.position[0] += local.x;
        t.position[1] += local.y;
        t.position[2] += local.z;
        // Spheres roll along the ground instead of sliding
        if (grounded && self.shape == Collider::Shape::Sphere && self.sphere.radius > 1e-4f) {
            Vec3 axis = cross(groundNormal, moved);
            float dist = length(axis);
            if (dist > 1e-6f) {
                float pos[3] = {t.position[0], t.position[1], t.position[2]}, scale[3] = {t.scale[0], t.scale[1], t.scale[2]};
                t.setMatrix(rotateAxis(axis * (1.f / dist), dist / self.sphere.radius) * t.matrix());
                std::copy(pos, pos + 3, t.position);
                std::copy(scale, scale + 3, t.scale);
            }
        }
    }
    // Bodies shoved by others moved in `world` only: bring their entities along
    for (const PhysicsWorld::Body& b : bodies) {
        if (!b.dynamic) continue;
        Entity* e = scene.find(b.entity);
        Mat4 now = scene.worldMatrix(e->id);
        Vec3 delta = Vec3{b.world.m[12], b.world.m[13], b.world.m[14]} - Vec3{now.m[12], now.m[13], now.m[14]};
        if (length(delta) < 1e-6f) continue;
        Vec3 local = transformDir(inverseAffine(scene.parentWorldMatrix(e->id)), delta);
        e->transform.position[0] += local.x;
        e->transform.position[1] += local.y;
        e->transform.position[2] += local.z;
    }
    return impacts;
}
