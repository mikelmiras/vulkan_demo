#include "physics/dynamics.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

namespace {

// ---- Layers: things that never move don't collide with each other
namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0, kMoving = 1, kCount = 2;
}
const JPH::BroadPhaseLayer kBroadStatic(0), kBroadMoving(1);

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::kStatic ? kBroadStatic : kBroadMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == kBroadStatic ? "static" : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == Layers::kMoving || broad == kBroadMoving;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::kMoving || b == Layers::kMoving;
    }
};

// New contacts, for impact sounds
class ContactRecorder final : public JPH::ContactListener {
public:
    std::mutex mutex;
    std::unordered_map<EntityId, Impact> impacts;   // the hardest per body this step

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m,
                        JPH::ContactSettings&) override {
        JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        float closing = (a.GetPointVelocity(p) - b.GetPointVelocity(p)).Dot(m.mWorldSpaceNormal);
        if (closing <= 1.5f) return;
        std::lock_guard<std::mutex> lock(mutex);
        for (const JPH::Body* body : {&a, &b}) {
            if (!body->IsDynamic()) continue;
            EntityId id = (EntityId)body->GetUserData();
            Impact& i = impacts[id];
            if (closing > i.speed) i = {id, {(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()}, closing};
        }
    }
};

void initJolt() {
    static bool done = false;
    if (done) return;
    done = true;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();   // (kept for the life of the program)
    JPH::RegisterTypes();
}

JPH::Vec3 toJ(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 fromJ(JPH::Vec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
JPH::Quat toJ(Quat q) { return {q.x, q.y, q.z, q.w}; }
Quat fromJ(JPH::Quat q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

// What a body's shape is made from: when any of it changes the body is made again
struct ShapeKey {
    int shape = 0, mesh = -1;
    bool dynamic = false;
    float values[9] = {};   // scale, centre, size, radius / height
    // Within a hair: transforms read back through Euler angles wobble in the last bits, which mustn't count as a
    // new shape (making the body again would lose its contacts and its rest)
    bool operator==(const ShapeKey& o) const {
        if (shape != o.shape || mesh != o.mesh || dynamic != o.dynamic) return false;
        for (int i = 0; i < 9; ++i)
            if (std::fabs(values[i] - o.values[i]) > 1e-4f * std::fmax(1.f, std::fabs(values[i]))) return false;
        return true;
    }
};

// A world matrix as position, rotation and (per axis) scale
struct Placement {
    Vec3 position, scale;
    Quat rotation;
};

Placement place(const Mat4& m) {
    Placement p;
    p.position = {m.m[12], m.m[13], m.m[14]};
    p.scale = {length(Vec3{m.m[0], m.m[1], m.m[2]}), length(Vec3{m.m[4], m.m[5], m.m[6]}),
               length(Vec3{m.m[8], m.m[9], m.m[10]})};
    p.rotation = rotationOf(m);
    return p;
}

bool near(Vec3 a, Vec3 b, float eps) { return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps; }
bool near(Quat a, Quat b, float eps) {
    return std::fabs(std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1.f) <= eps;
}

}  // namespace

struct RigidBodies::Impl {
    struct Tracked {
        JPH::BodyID id;
        ShapeKey key;
        bool dynamic = false, kinematic = false, moving = false;
        Vec3 position, velocity, angularVelocity;   // as last written to (or read from) the entity
        Quat rotation;
        bool seen = false;
    };

    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    BroadPhaseLayers broadPhase;
    ObjectVsBroadPhase objectVsBroadPhase;
    ObjectPairs objectPairs;
    JPH::PhysicsSystem system;
    ContactRecorder contacts;
    std::unordered_map<EntityId, Tracked> bodies;
    std::vector<std::pair<ShapeKey, JPH::RefConst<JPH::Shape>>> shapeCache;   // identical colliders share a shape
    std::unordered_map<uint64_t, std::vector<size_t>> shapeIndex;              // rough hash -> cache entries
    int added = 0;                                                             // since the broad-phase was optimised

    Impl() {
        system.Init(65536, 0, 65536, 16384, broadPhase, objectVsBroadPhase, objectPairs);
        system.SetContactListener(&contacts);
        JPH::PhysicsSettings settings;
        settings.mPenetrationSlop = 0.002f;   // (default 2 cm: things resting on each other would visibly sink)
        settings.mLinearCastThreshold = 0.1f;  // sweep bodies moving a tenth of their size per step: no sinking in
                                               // on hard landings
        system.SetPhysicsSettings(settings);
    }

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }

    void remove(Tracked& t) {
        bi().RemoveBody(t.id);
        bi().DestroyBody(t.id);
    }

    // The collider as a Jolt shape, with the entity's scale baked in (Jolt bodies only move and rotate)
    JPH::RefConst<JPH::Shape> makeShape(const Entity& e, const MeshLibrary& meshes, Vec3 s, bool dynamic) {
        const Collider& c = *e.collider;
        JPH::RefConst<JPH::Shape> inner;
        Vec3 centre{c.center.x * s.x, c.center.y * s.y, c.center.z * s.z};
        switch (c.shape) {
            case Collider::Shape::Box: {
                Vec3 h{std::fmax(c.size.x * s.x * 0.5f, 0.01f), std::fmax(c.size.y * s.y * 0.5f, 0.01f),
                       std::fmax(c.size.z * s.z * 0.5f, 0.01f)};
                float convex = std::fmin(0.05f, 0.5f * std::fmin(h.x, std::fmin(h.y, h.z)));
                inner = new JPH::BoxShape(toJ(h), convex);
                break;
            }
            case Collider::Shape::Sphere:
                inner = new JPH::SphereShape(std::fmax(c.radius * std::fmax(s.x, std::fmax(s.y, s.z)), 0.01f));
                break;
            case Collider::Shape::Capsule: {
                float r = std::fmax(c.radius * std::fmax(s.x, s.z), 0.01f);
                float half = std::fmax(c.height * 0.5f * s.y - r, 0.001f);
                inner = new JPH::CapsuleShape(half, r);
                break;
            }
            case Collider::Shape::Mesh: {
                const Mesh& m = meshes.get(e.meshRenderer->mesh);
                const Vertex* v = &meshes.vertices[(size_t)m.vertexOffset];
                const uint32_t* idx = &meshes.indices[m.firstIndex];
                auto scaled = [&](uint32_t i) { return JPH::Float3(v[i].pos.x * s.x, v[i].pos.y * s.y, v[i].pos.z * s.z); };
                if (dynamic) {   // moving meshes collide as their convex hull
                    JPH::Array<JPH::Vec3> points;
                    for (uint32_t i = 0; i < m.indexCount; ++i) {
                        JPH::Float3 f = scaled(idx[i]);
                        points.push_back(JPH::Vec3(f.x, f.y, f.z));
                    }
                    JPH::ConvexHullShapeSettings hull(points);
                    auto r = hull.Create();
                    if (r.HasError()) return nullptr;
                    inner = r.Get();
                } else {
                    JPH::TriangleList tris;
                    for (uint32_t i = 0; i + 2 < m.indexCount; i += 3)
                        tris.push_back(JPH::Triangle(scaled(idx[i]), scaled(idx[i + 1]), scaled(idx[i + 2])));
                    JPH::MeshShapeSettings mesh(tris);
                    auto r = mesh.Create();
                    if (r.HasError()) return nullptr;
                    inner = r.Get();
                }
                centre = {};   // (the triangles are already in place)
                break;
            }
        }
        if (centre.x == 0.f && centre.y == 0.f && centre.z == 0.f) return inner;
        auto r = JPH::RotatedTranslatedShapeSettings(toJ(centre), JPH::Quat::sIdentity(), inner).Create();
        return r.HasError() ? nullptr : r.Get();
    }

    JPH::RefConst<JPH::Shape> sharedShape(const Entity& e, const MeshLibrary& meshes, const Placement& p, const ShapeKey& key) {
        uint64_t h = (uint64_t)key.shape * 31u + (uint64_t)(key.mesh + 1) * 131u + (key.dynamic ? 7u : 0u);
        for (float v : key.values) h = h * 1000003u + (uint64_t)(int64_t)std::lround(v * 1000.f);
        auto it = shapeIndex.find(h);
        if (it != shapeIndex.end())
            for (size_t i : it->second)
                if (shapeCache[i].first == key) return shapeCache[i].second;
        JPH::RefConst<JPH::Shape> shape = makeShape(e, meshes, p.scale, key.dynamic);
        if (!shape) return nullptr;
        shapeIndex[h].push_back(shapeCache.size());
        shapeCache.emplace_back(key, shape);
        return shape;
    }

    bool create(Tracked& t, const Entity& e, const MeshLibrary& meshes, const Placement& p, const ShapeKey& key) {
        bool dynamic = key.dynamic;
        JPH::RefConst<JPH::Shape> shape = sharedShape(e, meshes, p, key);
        if (!shape) return false;
        JPH::BodyCreationSettings s(shape, JPH::RVec3(p.position.x, p.position.y, p.position.z), toJ(p.rotation),
                                    dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                    dynamic ? Layers::kMoving : Layers::kStatic);
        s.mUserData = e.id;
        s.mAllowDynamicOrKinematic = !dynamic;   // a static one becomes kinematic once game code moves it
        s.mFriction = 0.6f;
        if (dynamic) {
            const RigidBody& rb = *e.rigidBody;
            s.mFriction = rb.friction;
            s.mRestitution = rb.bounciness;
            s.mLinearDamping = rb.damping;
            s.mAngularDamping = 0.05f;
            s.mGravityFactor = rb.useGravity ? 1.f : 0.f;
            s.mLinearVelocity = toJ(rb.velocity);
            s.mAngularVelocity = toJ(rb.angularVelocity);
            s.mMotionQuality = JPH::EMotionQuality::LinearCast;   // fast bodies don't tunnel through thin walls
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            s.mMassPropertiesOverride.mMass = std::fmax(rb.mass, 0.001f);
            if (rb.lockRotation)
                s.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
        }
        JPH::Body* body = bi().CreateBody(s);
        if (!body) return false;   // out of bodies
        bi().AddBody(body->GetID(), dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        ++added;
        t = {};
        t.id = body->GetID();
        t.key = key;
        t.dynamic = dynamic;
        t.position = p.position;
        t.rotation = p.rotation;
        if (dynamic) {
            t.velocity = e.rigidBody->velocity;
            t.angularVelocity = e.rigidBody->angularVelocity;
        }
        return true;
    }
};

RigidBodies::RigidBodies() {
    initJolt();
    impl_ = std::make_unique<Impl>();
}

RigidBodies::~RigidBodies() { clear(); }

size_t RigidBodies::bodyCount() const { return impl_->bodies.size(); }

void RigidBodies::clear() {
    for (auto& [id, t] : impl_->bodies) impl_->remove(t);
    impl_->bodies.clear();
    impl_->shapeCache.clear();
    impl_->shapeIndex.clear();
}

std::vector<Impact> RigidBodies::step(Scene& scene, const MeshLibrary& meshes, float dt, const DynamicsSettings& settings) {
    Impl& im = *impl_;
    im.system.SetGravity(toJ(settings.gravity));
    std::vector<Mat4> worlds = scene.worldMatrices();
    for (auto& [id, t] : im.bodies) t.seen = false;

    // ---- What the scene looks like now: new, changed and moved bodies
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.collider || e.collider->trigger) continue;
        if (e.collider->shape == Collider::Shape::Mesh && !e.meshRenderer) continue;
        Placement p = place(worlds[i]);
        ShapeKey key;
        key.shape = (int)e.collider->shape;
        key.mesh = e.collider->shape == Collider::Shape::Mesh ? e.meshRenderer->mesh : -1;
        key.dynamic = e.rigidBody.has_value();
        const Collider& c = e.collider.value();
        const float values[9] = {p.scale.x, p.scale.y, p.scale.z, c.center.x, c.center.y, c.center.z,
                                 c.size.x + c.size.y * 3.1f + c.size.z * 7.3f, c.radius, c.height};
        std::memcpy(key.values, values, sizeof(values));

        auto it = im.bodies.find(e.id);
        if (it != im.bodies.end() && !(it->second.key == key)) {   // reshaped: start over
            im.remove(it->second);
            im.bodies.erase(it);
            it = im.bodies.end();
        }
        if (it == im.bodies.end()) {
            Impl::Tracked t;
            if (im.create(t, e, meshes, p, key)) {
                t.seen = true;
                im.bodies.emplace(e.id, t);
            }
            continue;
        }
        Impl::Tracked& t = it->second;
        t.seen = true;
        bool moved = !near(p.position, t.position, 1e-5f) || !near(p.rotation, t.rotation, 1e-6f);
        if (t.dynamic) {   // game code teleported it or changed its velocity: tell Jolt
            const RigidBody& rb = *e.rigidBody;
            if (moved) im.bi().SetPositionAndRotation(t.id, JPH::RVec3(p.position.x, p.position.y, p.position.z),
                                                      toJ(p.rotation), JPH::EActivation::Activate);
            if (!near(rb.velocity, t.velocity, 1e-6f)) im.bi().SetLinearVelocity(t.id, toJ(rb.velocity));
            if (!near(rb.angularVelocity, t.angularVelocity, 1e-6f)) im.bi().SetAngularVelocity(t.id, toJ(rb.angularVelocity));
            t.position = p.position;
            t.rotation = p.rotation;
            t.velocity = rb.velocity;
            t.angularVelocity = rb.angularVelocity;
        } else if (moved || t.moving) {
            // Moved by game code (or just stopped): kinematic, so it pushes what it moves into
            if (!t.kinematic) {
                im.bi().SetObjectLayer(t.id, Layers::kMoving);
                im.bi().SetMotionType(t.id, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
                t.kinematic = true;
            }
            im.bi().MoveKinematic(t.id, JPH::RVec3(p.position.x, p.position.y, p.position.z), toJ(p.rotation), dt);
            t.position = p.position;
            t.rotation = p.rotation;
            t.moving = moved;
        }
    }
    for (auto it = im.bodies.begin(); it != im.bodies.end();) {   // entities (or colliders) that are gone
        if (it->second.seen) { ++it; continue; }
        im.remove(it->second);
        it = im.bodies.erase(it);
    }

    if (im.added > 256) {   // many new bodies (a scene starting): rebuild the broad-phase's trees once
        im.system.OptimizeBroadPhase();
        im.added = 0;
    }

    // ---- Simulate
    if (dt <= 0.f) return {};   // (just bringing the bodies up to date)
    im.contacts.impacts.clear();
    im.system.Update(dt, 1, &im.temp, &im.jobs);

    // ---- Write the moving bodies back
    for (auto& [id, t] : im.bodies) {
        if (!t.dynamic) continue;
        int index = scene.indexOf(id);
        if (index < 0) continue;
        Entity& e = scene.entities[(size_t)index];
        JPH::RVec3 jp;
        JPH::Quat jq;
        im.bi().GetPositionAndRotation(t.id, jp, jq);
        Vec3 pos{(float)jp.GetX(), (float)jp.GetY(), (float)jp.GetZ()};
        Quat rot = fromJ(jq.Normalized());
        Placement was = place(worlds[(size_t)index]);
        if (!near(pos, t.position, 1e-7f) || !near(rot, t.rotation, 1e-9f)) {
            Mat4 world = composeTRS(pos, rot, was.scale);
            int parent = scene.indexOf(e.parent);
            e.transform.setMatrix(parent >= 0 ? inverseAffine(worlds[(size_t)parent]) * world : world);
        }
        e.rigidBody->velocity = fromJ(im.bi().GetLinearVelocity(t.id));
        e.rigidBody->angularVelocity = fromJ(im.bi().GetAngularVelocity(t.id));
        t.velocity = e.rigidBody->velocity;
        t.angularVelocity = e.rigidBody->angularVelocity;
        // A written-back transform reads back slightly differently (Euler angles): remember what it now says, so
        // the next step doesn't take the difference for game code moving it
        int parent = scene.indexOf(e.parent);
        Placement now = place(parent >= 0 ? worlds[(size_t)parent] * e.transform.matrix() : e.transform.matrix());
        t.position = now.position;
        t.rotation = now.rotation;
    }

    std::vector<Impact> impacts;
    for (auto& [id, i] : im.contacts.impacts) impacts.push_back(i);
    return impacts;
}
