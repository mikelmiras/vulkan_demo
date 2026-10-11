#include "physics/physics_world.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

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
        b.dynamic = e.rigidBody.has_value() && !c.trigger;
        bodies_.push_back(b);
    }
    buildTree();
}

// ---------------------------------------------------------------------------------------------
// Broad-phase
// ---------------------------------------------------------------------------------------------
void PhysicsWorld::buildTree() {
    nodes_.clear();
    staticIdx_.clear();
    dynamicIdx_.clear();
    EntityId maxId = 0;
    for (const Body& b : bodies_) maxId = std::max(maxId, b.entity);
    byEntity_.assign(bodies_.empty() ? 0 : (size_t)maxId + 1, -1);
    for (size_t i = 0; i < bodies_.size(); ++i) {
        (bodies_[i].dynamic ? dynamicIdx_ : staticIdx_).push_back((int)i);
        int& slot = byEntity_[bodies_[i].entity];
        if (slot < 0) slot = (int)i;   // the first body wins, as find() always did
    }
    if (!staticIdx_.empty()) {
        prims_.resize(bodies_.size());
        for (int i : staticIdx_) {
            const Body& b = bodies_[(size_t)i];
            prims_[(size_t)i] = {b.aabbMin, b.aabbMax, b.aabbMin + b.aabbMax};
        }
        nodes_.reserve(staticIdx_.size() / 2 + 1);
        buildNode(0, (int)staticIdx_.size(), 0);
    }
}

// Median split along the longest axis of the centres: O(n log n) and good enough for scenes that are rebuilt
// every update. Reads the compact prims_, not the (large) bodies, to stay in cache.
int PhysicsWorld::buildNode(int first, int count, int depth) {
    int index = (int)nodes_.size();
    nodes_.emplace_back();
    Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    Vec3 clo = lo, chi = hi;
    for (int i = first; i < first + count; ++i) {
        const Prim& p = prims_[(size_t)staticIdx_[(size_t)i]];
        growBounds(lo, hi, p.lo);
        growBounds(lo, hi, p.hi);
        growBounds(clo, chi, p.center2);
    }
    nodes_[(size_t)index].lo = lo;
    nodes_[(size_t)index].hi = hi;
    if (count <= 4 || depth > 48) {
        nodes_[(size_t)index].first = first;
        nodes_[(size_t)index].count = count;
        return index;
    }
    Vec3 ext = chi - clo;
    int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
    auto key = [&](int bi) {
        const Vec3& c = prims_[(size_t)bi].center2;
        return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
    };
    int half = count / 2;
    std::nth_element(staticIdx_.begin() + first, staticIdx_.begin() + first + half, staticIdx_.begin() + first + count,
                     [&](int a, int b) { return key(a) < key(b); });
    int left = buildNode(first, half, depth + 1);
    int right = buildNode(first + half, count - half, depth + 1);
    nodes_[(size_t)index].left = left;
    nodes_[(size_t)index].right = right;
    return index;
}

void PhysicsWorld::queryBox(Vec3 lo, Vec3 hi, std::vector<int>& out) const {
    if (nodes_.empty()) return;
    int stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes_[(size_t)stack[--top]];
        if (!boundsOverlap(n.lo, n.hi, lo, hi)) continue;
        if (n.count > 0) {
            for (int i = n.first; i < n.first + n.count; ++i) {
                const Prim& p = prims_[(size_t)staticIdx_[(size_t)i]];
                if (boundsOverlap(p.lo, p.hi, lo, hi)) out.push_back(staticIdx_[(size_t)i]);
            }
        } else if (top + 2 <= 128) {
            stack[top++] = n.left;
            stack[top++] = n.right;
        }
    }
}

namespace {
// Does the ray (origin, 1/dir) cross the box before maxT?
bool rayHitsBounds(Vec3 o, Vec3 inv, float maxT, Vec3 lo, Vec3 hi) {
    float t0 = 0.f, t1 = maxT;
    const float os[3] = {o.x, o.y, o.z}, is[3] = {inv.x, inv.y, inv.z}, ls[3] = {lo.x, lo.y, lo.z}, hs[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        float a = (ls[k] - os[k]) * is[k], b = (hs[k] - os[k]) * is[k];
        if (a > b) std::swap(a, b);
        if (!(a <= t1 && b >= t0)) {
            // NaN from 0 * inf (origin on the slab plane, ray parallel to it): inside the slab, keep going
            if (std::isnan(a) || std::isnan(b)) continue;
            return false;
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    return true;
}
}  // namespace

void PhysicsWorld::queryRay(Vec3 origin, Vec3 invDir, float maxT, std::vector<int>& out) const {
    if (nodes_.empty()) return;
    int stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes_[(size_t)stack[--top]];
        if (!rayHitsBounds(origin, invDir, maxT, n.lo, n.hi)) continue;
        if (n.count > 0) {
            for (int i = n.first; i < n.first + n.count; ++i) out.push_back(staticIdx_[(size_t)i]);
        } else if (top + 2 <= 128) {
            stack[top++] = n.left;
            stack[top++] = n.right;
        }
    }
}

void PhysicsWorld::Body::translate(Vec3 d) {
    box.center += d;
    sphere.center += d;
    capsule.a += d;
    capsule.b += d;
    aabbMin += d;
    aabbMax += d;
    world.m[12] += d.x;
    world.m[13] += d.y;
    world.m[14] += d.z;
}

bool PhysicsWorld::contact(const Body& a, const Body& b, Contact& out) const {
    if (!boundsOverlap(a.aabbMin, a.aabbMax, b.aabbMin, b.aabbMax)) return false;
    bool aBoxy = a.shape == Collider::Shape::Box || a.shape == Collider::Shape::Mesh;
    bool bBoxy = b.shape == Collider::Shape::Box;
    if (aBoxy && bBoxy) return obbContact(a.box, b.box, out);
    Contact deepest;
    auto test = [&](const Sphere& s) {
        Contact c;
        if (contact(s, b, c) && c.depth > deepest.depth) deepest = c;
        return true;
    };
    Capsule cap;
    if (capsuleOf(a, cap)) {
        forEachCapsuleSphere(cap, test);
    } else {   // a box against a sphere / capsule / mesh: spheres in its corners and middle
        float r = std::min({a.box.half.x, a.box.half.y, a.box.half.z});
        test(Sphere{a.box.center, r});
        for (int i = 0; i < 8; ++i) {
            Vec3 p = a.box.center;
            const float h[3] = {a.box.half.x - r * 0.5f, a.box.half.y - r * 0.5f, a.box.half.z - r * 0.5f};
            for (int k = 0; k < 3; ++k) p += a.box.axis[k] * (((i >> k) & 1) ? h[k] : -h[k]);
            test(Sphere{p, r * 0.5f});
        }
    }
    if (deepest.depth <= 0.f) return false;
    out = deepest;
    return true;
}

const PhysicsWorld::Body* PhysicsWorld::find(EntityId id) const {
    int i = id < byEntity_.size() ? byEntity_[id] : -1;
    return i >= 0 ? &bodies_[(size_t)i] : nullptr;
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
    Vec3 inv{1.f / dir.x, 1.f / dir.y, 1.f / dir.z};
    std::vector<int> candidates;
    queryRay(origin, inv, maxDistance, candidates);
    for (int i : dynamicIdx_)
        if (rayHitsBounds(origin, inv, maxDistance, bodies_[(size_t)i].aabbMin, bodies_[(size_t)i].aabbMax))
            candidates.push_back(i);
    std::sort(candidates.begin(), candidates.end());   // body order: equally distant hits resolve as they always did
    for (int ci : candidates) {
        const Body& b = bodies_[(size_t)ci];
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
                Mat4 toLocal = inverseAffine(b.world);
                Vec3 lo = transformPoint(toLocal, origin), ld = transformDir(toLocal, dir), ln;
                t = intersectMesh(*meshes_, b.meshId, lo, ld, &ln);
                if (t >= 0.f) {
                    n = transformNormal(toLocal, ln);
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
    Vec3 r{radius, radius, radius};
    forEachNear(center - r, center + r, [&](const Body& b) {
        if (b.entity == ignore || (b.trigger && !includeTriggers)) return;
        Contact c;
        if (contact(Sphere{center, radius}, b, c)) out.push_back(b.entity);
    });
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
            const Body* deepestBody = nullptr;
            Vec3 rr{c.radius, c.radius, c.radius};
            Vec3 lo = Vec3{std::min(c.a.x, c.b.x), std::min(c.a.y, c.b.y), std::min(c.a.z, c.b.z)} - rr;
            Vec3 hi = Vec3{std::max(c.a.x, c.b.x), std::max(c.a.y, c.b.y), std::max(c.a.z, c.b.z)} + rr;
            forEachNear(lo, hi, [&](const Body& b) {
                if (b.trigger || b.entity == ignore) return;
                forEachCapsuleSphere(c, [&](const Sphere& sp) {
                    Contact ct;
                    if (contact(sp, b, ct) && ct.depth > deepest.depth) {
                        deepest = ct;
                        deepestBody = &b;
                    }
                    return true;
                });
            });
            if (deepestBody && deepestBody->dynamic) {
                bool known = false;
                for (const auto& t : result.touched) known |= t.first == deepestBody->entity;
                if (!known) result.touched.emplace_back(deepestBody->entity, deepest.normal * -1.f);
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
        Vec3 rr{foot.radius, foot.radius, foot.radius};
        forEachNear(foot.center - rr, foot.center + rr, [&](const Body& b) {
            Contact ct;
            if (!result.grounded && !b.trigger && b.entity != ignore && contact(foot, b, ct) && ct.normal.y > 0.7f)
                result.grounded = true;
        });
    }
    result.displacement = c.a - capsule.a;
    return result;
}

std::vector<std::pair<EntityId, EntityId>> PhysicsWorld::triggerOverlaps() const {
    std::vector<std::pair<EntityId, EntityId>> out;
    for (const Body& t : bodies_) {
        if (!t.trigger) continue;
        forEachNear(t.aabbMin, t.aabbMax, [&](const Body& o) {
            if (!o.trigger && o.entity != t.entity && overlaps(t, o)) out.emplace_back(t.entity, o.entity);
        });
    }
    // In body order, as before the broad-phase (callbacks run in this order)
    std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return byEntity_[a.first] < byEntity_[b.first];
        return byEntity_[a.second] < byEntity_[b.second];
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
