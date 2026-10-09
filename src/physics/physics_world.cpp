#include "physics/physics_world.h"

#include <cfloat>

using namespace collision;

namespace {

Vec3 column(const Mat4& m, int c) { return {m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }

void growBounds(Vec3& lo, Vec3& hi, Vec3 p) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
}

void obbBounds(const Obb& b, Vec3& lo, Vec3& hi) {
    Vec3 ext;
    for (int i = 0; i < 3; ++i) {
        Vec3 a = b.axis[i] * (i == 0 ? b.half.x : i == 1 ? b.half.y : b.half.z);
        ext = ext + Vec3{std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)};
    }
    lo = b.center - ext;
    hi = b.center + ext;
}

bool boundsOverlap(Vec3 alo, Vec3 ahi, Vec3 blo, Vec3 bhi) {
    return alo.x <= bhi.x && ahi.x >= blo.x && alo.y <= bhi.y && ahi.y >= blo.y && alo.z <= bhi.z && ahi.z >= blo.z;
}

// Normal from object space to world space: inverse-transpose of the 3x3 part
Vec3 transformNormal(const Mat4& inv, Vec3 n) {
    return normalize({inv.m[0] * n.x + inv.m[1] * n.y + inv.m[2] * n.z,
                      inv.m[4] * n.x + inv.m[5] * n.y + inv.m[6] * n.z,
                      inv.m[8] * n.x + inv.m[9] * n.y + inv.m[10] * n.z});
}

constexpr float kSkin = 0.001f;   // keep resolved shapes this far apart, so resting contacts stay stable

}  // namespace

// ---------------------------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------------------------
void PhysicsWorld::build(const Scene& scene, const MeshLibrary& meshes) {
    meshes_ = &meshes;
    bodies_.clear();
    std::vector<Mat4> worlds = scene.worldMatrices();
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.collider) continue;
        const Collider& c = *e.collider;
        Body b;
        b.entity = e.id;
        b.shape = c.shape;
        b.trigger = c.trigger;
        b.world = worlds[i];
        const Mat4& m = b.world;
        Vec3 cols[3] = {column(m, 0), column(m, 1), column(m, 2)};
        float scale[3] = {length(cols[0]), length(cols[1]), length(cols[2])};

        Vec3 center = c.center, size = c.size;
        if (c.shape == Collider::Shape::Mesh) {
            if (!e.meshRenderer) continue;   // nothing to collide with
            b.meshId = e.meshRenderer->mesh;
            b.mesh = &meshes.get(b.meshId);
            center = (b.mesh->boundsMin + b.mesh->boundsMax) * 0.5f;
            size = b.mesh->boundsMax - b.mesh->boundsMin;
        }
        b.box.center = transformPoint(m, center);
        for (int k = 0; k < 3; ++k) b.box.axis[k] = scale[k] > 1e-9f ? cols[k] * (1.f / scale[k]) : Vec3{};
        b.box.half = {size.x * 0.5f * scale[0], size.y * 0.5f * scale[1], size.z * 0.5f * scale[2]};

        b.sphere = {b.box.center, c.radius * std::max({scale[0], scale[1], scale[2]})};
        float r = c.radius * std::max(scale[0], scale[2]);
        float halfSeg = std::max(c.height * 0.5f * scale[1] - r, 0.f);
        Vec3 up = b.box.axis[1];
        b.capsule = {b.box.center - up * halfSeg, b.box.center + up * halfSeg, r};

        switch (c.shape) {
            case Collider::Shape::Box:
            case Collider::Shape::Mesh: obbBounds(b.box, b.aabbMin, b.aabbMax); break;
            case Collider::Shape::Sphere:
                b.aabbMin = b.sphere.center - Vec3{b.sphere.radius, b.sphere.radius, b.sphere.radius};
                b.aabbMax = b.sphere.center + Vec3{b.sphere.radius, b.sphere.radius, b.sphere.radius};
                break;
            case Collider::Shape::Capsule: {
                Vec3 rr{r, r, r};
                b.aabbMin = b.aabbMax = b.capsule.a;
                growBounds(b.aabbMin, b.aabbMax, b.capsule.b);
                b.aabbMin = b.aabbMin - rr;
                b.aabbMax = b.aabbMax + rr;
                break;
            }
        }
        bodies_.push_back(b);
    }
}

const PhysicsWorld::Body* PhysicsWorld::find(EntityId id) const {
    for (const Body& b : bodies_)
        if (b.entity == id) return &b;
    return nullptr;
}

bool PhysicsWorld::capsuleOf(const Body& b, Capsule& out) {
    if (b.shape == Collider::Shape::Capsule) { out = b.capsule; return true; }
    if (b.shape == Collider::Shape::Sphere) { out = {b.sphere.center, b.sphere.center, b.sphere.radius}; return true; }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------------------------
bool PhysicsWorld::contact(const Sphere& s, const Body& b, Contact& out) const {
    Vec3 r{s.radius, s.radius, s.radius};
    if (!boundsOverlap(s.center - r, s.center + r, b.aabbMin, b.aabbMax)) return false;
    switch (b.shape) {
        case Collider::Shape::Box: return sphereVsObb(s, b.box, out);
        case Collider::Shape::Sphere: return sphereVsSphere(s, b.sphere, out);
        case Collider::Shape::Capsule: return sphereVsCapsule(s, b.capsule, out);
        case Collider::Shape::Mesh: {
            // Triangles go to world space on demand: only for meshes the sphere is already near
            const Mesh& m = *b.mesh;
            const Vertex* v = &meshes_->vertices[(size_t)m.vertexOffset];
            const uint32_t* idx = &meshes_->indices[m.firstIndex];
            bool any = false;
            for (uint32_t i = 0; i < m.indexCount; i += 3) {
                Vec3 p0 = transformPoint(b.world, v[idx[i]].pos), p1 = transformPoint(b.world, v[idx[i + 1]].pos),
                     p2 = transformPoint(b.world, v[idx[i + 2]].pos);
                Contact c;
                if (sphereVsTriangle(s, p0, p1, p2, c) && c.depth > out.depth) {
                    out = c;
                    any = true;
                }
            }
            return any;
        }
    }
    return false;
}

bool PhysicsWorld::overlaps(const Body& a, const Body& b) const {
    if (!boundsOverlap(a.aabbMin, a.aabbMax, b.aabbMin, b.aabbMax)) return false;
    Capsule ca, cb;
    if (capsuleOf(a, ca)) {   // spheres and capsules: a row of spheres against the other shape
        bool hit = false;
        forEachCapsuleSphere(ca, [&](const Sphere& s) {
            Contact c;
            hit = contact(s, b, c);
            return !hit;
        });
        return hit;
    }
    if (capsuleOf(b, cb)) return overlaps(b, a);
    return obbVsObb(a.box, b.box);   // boxes, and meshes by their bounds
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------
bool PhysicsWorld::raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore) const {
    float len = length(dir);
    if (len < 1e-9f) return false;
    dir = dir * (1.f / len);
    float best = maxDistance;
    bool found = false;
    for (const Body& b : bodies_) {
        if (b.trigger || b.entity == ignore) continue;
        Vec3 n;
        float t = -1.f;
        switch (b.shape) {
            case Collider::Shape::Box: t = rayVsObb(origin, dir, b.box, n); break;
            case Collider::Shape::Sphere: t = rayVsSphere(origin, dir, b.sphere, n); break;
            case Collider::Shape::Capsule: {
                // Closest approach of the ray to the segment, then a sphere test there (good enough for characters)
                Vec3 q = closestPointOnSegment(origin + dir * std::max(dot(b.capsule.a - origin, dir), 0.f),
                                               b.capsule.a, b.capsule.b);
                t = rayVsSphere(origin, dir, Sphere{q, b.capsule.radius}, n);
                break;
            }
            case Collider::Shape::Mesh: {
                Vec3 bn;
                if (rayVsObb(origin, dir, b.box, bn) < 0.f) break;
                Mat4 inv = inverseAffine(b.world);
                Vec3 lo = transformPoint(inv, origin), ld = transformDir(inv, dir), ln;
                t = intersectMesh(*meshes_, b.meshId, lo, ld, &ln);
                if (t >= 0.f) {
                    n = transformNormal(inv, ln);
                    if (dot(n, dir) > 0.f) n = n * -1.f;   // meshes are double-sided
                }
                break;
            }
        }
        if (t >= 0.f && t < best) {
            best = t;
            hit = {b.entity, origin + dir * t, n, t};
            found = true;
        }
    }
    return found;
}

std::vector<EntityId> PhysicsWorld::overlapSphere(Vec3 center, float radius, bool includeTriggers, EntityId ignore) const {
    std::vector<EntityId> out;
    for (const Body& b : bodies_) {
        if (b.entity == ignore || (b.trigger && !includeTriggers)) continue;
        Contact c;
        if (contact(Sphere{center, radius}, b, c)) out.push_back(b.entity);
    }
    return out;
}

CharacterMove PhysicsWorld::moveCapsule(const Capsule& capsule, Vec3 delta, EntityId ignore) const {
    CharacterMove result;
    Capsule c = capsule;
    // Sub-steps no longer than half the radius, so a fast move can't jump over a thin wall
    int steps = std::max(1, (int)std::ceil(length(delta) / std::max(capsule.radius * 0.5f, 1e-3f)));
    steps = std::min(steps, 64);
    Vec3 step = delta * (1.f / (float)steps);
    for (int s = 0; s < steps; ++s) {
        c.a += step;
        c.b += step;
        for (int iter = 0; iter < 4; ++iter) {   // resolve the deepest contact, a few times
            Contact deepest;
            for (const Body& b : bodies_) {
                if (b.trigger || b.entity == ignore) continue;
                forEachCapsuleSphere(c, [&](const Sphere& sp) {
                    Contact ct;
                    if (contact(sp, b, ct) && ct.depth > deepest.depth) deepest = ct;
                    return true;
                });
            }
            if (deepest.depth <= 0.f) break;
            Vec3 push = deepest.normal * (deepest.depth + kSkin);
            c.a += push;
            c.b += push;
            if (deepest.normal.y > 0.7f) result.grounded = true;
            else if (deepest.normal.y < -0.7f) result.hitCeiling = true;
            else result.hitWall = true;
        }
    }
    // Resting on the ground leaves a skin-width gap: probe just below so standing still still counts as grounded
    if (!result.grounded) {
        Sphere foot{(c.a.y < c.b.y ? c.a : c.b) - Vec3{0, kSkin * 4.f, 0}, c.radius};
        for (const Body& b : bodies_) {
            Contact ct;
            if (!b.trigger && b.entity != ignore && contact(foot, b, ct) && ct.normal.y > 0.7f) {
                result.grounded = true;
                break;
            }
        }
    }
    result.displacement = c.a - capsule.a;
    return result;
}

std::vector<std::pair<EntityId, EntityId>> PhysicsWorld::triggerOverlaps() const {
    std::vector<std::pair<EntityId, EntityId>> out;
    for (const Body& t : bodies_) {
        if (!t.trigger) continue;
        for (const Body& o : bodies_)
            if (!o.trigger && o.entity != t.entity && overlaps(t, o)) out.emplace_back(t.entity, o.entity);
    }
    return out;
}
