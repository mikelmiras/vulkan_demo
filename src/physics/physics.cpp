#include "physics/physics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

namespace {

// ---- Layers. A Jolt object layer is the entity's layer (0..31) times two, plus one if the body can move: things
// that never move don't collide with each other, and the scene's layer matrix (LayerSettings) says which layers meet.
JPH::ObjectLayer objectLayer(int layer, bool moving) { return (JPH::ObjectLayer)(((layer & 31) << 1) | (moving ? 1 : 0)); }
bool isMoving(JPH::ObjectLayer l) { return (l & 1) != 0; }
int userLayer(JPH::ObjectLayer l) { return (int)(l >> 1); }
const JPH::BroadPhaseLayer kBroadStatic(0), kBroadMoving(1);

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return isMoving(layer) ? kBroadMoving : kBroadStatic;
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
        return isMoving(layer) || broad == kBroadMoving;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    uint32_t collides[32];   // the scene's LayerSettings::collides
    ObjectPairs() {
        for (uint32_t& c : collides) c = ~0u;
    }
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return (isMoving(a) || isMoving(b)) && ((collides[userLayer(a)] >> userLayer(b)) & 1u);
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
    static std::once_flag once;   // (worlds are also made on loading threads)
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();   // (kept for the life of the program)
        JPH::RegisterTypes();
    });
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

uint64_t shapeHash(const ShapeKey& key) {
    uint64_t h = (uint64_t)key.shape * 31u + (uint64_t)(key.mesh + 1) * 131u + (key.dynamic ? 7u : 0u);
    for (float v : key.values) h = h * 1000003u + (uint64_t)(int64_t)std::lround(v * 1000.f);
    return h;
}

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

// Which bodies a query sees: not sensors (unless asked), not the entity asking
class QueryFilter final : public JPH::BodyFilter {
public:
    QueryFilter(bool sensors, EntityId ignore, uint32_t layers = ~0u) : sensors_(sensors), ignore_(ignore), layers_(layers) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (!sensors_ && body.IsSensor()) return false;
        if (!((layers_ >> userLayer(body.GetObjectLayer())) & 1u)) return false;
        return ignore_ == kNoEntity || (EntityId)body.GetUserData() != ignore_;
    }
private:
    bool sensors_;
    EntityId ignore_;
    uint32_t layers_;
};

}  // namespace

struct Physics::Impl {
    struct Tracked {
        JPH::BodyID id;
        ShapeKey key;
        bool dynamic = false, kinematic = false, moving = false;
        uint8_t layer = 0;
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
    struct Character {
        JPH::Ref<JPH::CharacterVirtual> character;
        ShapeKey key;
        Vec3 position;   // as last written to the entity (its world origin)
        bool seen = true;
    };
    std::unordered_map<EntityId, Character> characters;
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
        for (auto it = joints.begin(); it != joints.end();) {   // (a constraint mustn't outlive its bodies)
            if (it->second.c->GetBody1()->GetID() == t.id || it->second.c->GetBody2()->GetID() == t.id) {
                system.RemoveConstraint(it->second.c);
                it = joints.erase(it);
            } else {
                ++it;
            }
        }
        bi().RemoveBody(t.id);
        bi().DestroyBody(t.id);
    }

    // ---- Joints
    struct JointState {
        JPH::Ref<JPH::TwoBodyConstraint> c;
        uint64_t key = 0;
        Joint::Type type;
        float breakForce = 0.f;
    };
    std::unordered_map<EntityId, JointState> joints;
    JPH::Ref<JPH::GroupFilterTable> groups;   // pairs of jointed bodies that don't collide
    uint64_t groupsKey = 0;
    std::vector<JPH::BodyID> grouped;

    // The entity `name` refers to from `self`: under its parent first, then its grandparent's, ..., then anywhere
    static EntityId resolve(const Scene& scene, const Entity& self, const std::string& name) {
        for (EntityId p = self.parent; p != kNoEntity;) {
            std::unordered_map<std::string, EntityId> names = scene.namesUnder(p);
            if (auto it = names.find(name); it != names.end() && it->second != self.id) return it->second;
            const Entity* pe = scene.find(p);
            p = pe ? pe->parent : kNoEntity;
        }
        const Entity* any = scene.findByName(name);
        return any && any->id != self.id ? any->id : kNoEntity;
    }

    JPH::Ref<JPH::TwoBodyConstraintSettings> jointSettings(const Joint& j, const Mat4& world, Vec3 otherOrigin) {
        Vec3 at = transformPoint(world, j.anchor), axis = transformDir(world, j.axis);
        axis = length(axis) > 1e-6f ? normalize(axis) : Vec3{1, 0, 0};
        Vec3 normal = std::fabs(axis.y) < 0.9f ? normalize(cross(axis, {0, 1, 0})) : normalize(cross(axis, {1, 0, 0}));
        JPH::RVec3 p(at.x, at.y, at.z);
        bool limited = j.minLimit < j.maxLimit;
        switch (j.type) {
        case Joint::Type::Fixed: {
            auto* s = new JPH::FixedConstraintSettings;
            s->mPoint1 = s->mPoint2 = p;
            s->mAxisX1 = s->mAxisX2 = toJ(axis);
            s->mAxisY1 = s->mAxisY2 = toJ(normal);
            return s;
        }
        case Joint::Type::Ball: {
            auto* s = new JPH::PointConstraintSettings;
            s->mPoint1 = s->mPoint2 = p;
            return s;
        }
        case Joint::Type::Hinge: {
            auto* s = new JPH::HingeConstraintSettings;
            s->mPoint1 = s->mPoint2 = p;
            s->mHingeAxis1 = s->mHingeAxis2 = toJ(axis);
            s->mNormalAxis1 = s->mNormalAxis2 = toJ(normal);
            if (limited) {
                s->mLimitsMin = std::max(j.minLimit, -179.9f) * DEG2RAD;
                s->mLimitsMax = std::min(j.maxLimit, 179.9f) * DEG2RAD;
            }
            if (j.motorStrength > 0.f) s->mMotorSettings.SetTorqueLimit(j.motorStrength);
            return s;
        }
        case Joint::Type::Slider: {
            auto* s = new JPH::SliderConstraintSettings;
            s->mAutoDetectPoint = false;
            s->mPoint1 = s->mPoint2 = p;
            s->mSliderAxis1 = s->mSliderAxis2 = toJ(axis);
            s->mNormalAxis1 = s->mNormalAxis2 = toJ(normal);
            if (limited) {
                s->mLimitsMin = j.minLimit;
                s->mLimitsMax = j.maxLimit;
            }
            if (j.motorStrength > 0.f) s->mMotorSettings.SetForceLimit(j.motorStrength);
            return s;
        }
        case Joint::Type::Distance: {
            auto* s = new JPH::DistanceConstraintSettings;
            s->mPoint1 = JPH::RVec3(otherOrigin.x, otherOrigin.y, otherOrigin.z);
            s->mPoint2 = p;
            if (limited) {
                s->mMinDistance = j.minLimit;
                s->mMaxDistance = j.maxLimit;
            }
            return s;
        }
        case Joint::Type::Cone: {
            auto* s = new JPH::SwingTwistConstraintSettings;
            s->mPosition1 = s->mPosition2 = p;
            s->mTwistAxis1 = s->mTwistAxis2 = toJ(axis);
            s->mPlaneAxis1 = s->mPlaneAxis2 = toJ(normal);
            s->mNormalHalfConeAngle = s->mPlaneHalfConeAngle = std::clamp(j.coneAngle, 0.f, 179.f) * DEG2RAD;
            s->mTwistMinAngle = (limited ? j.minLimit : -20.f) * DEG2RAD;
            s->mTwistMaxAngle = (limited ? j.maxLimit : 20.f) * DEG2RAD;
            return s;
        }
        }
        return nullptr;
    }

    // Makes, keeps or remakes each joint's constraint (when it, or one of its bodies, changed)
    void syncJoints(Scene& scene, const std::vector<Mat4>& worlds) {
        std::vector<EntityId> seen;
        std::vector<std::pair<JPH::BodyID, JPH::BodyID>> apart;
        for (size_t i = 0; i < scene.entities.size(); ++i) {
            const Entity& e = scene.entities[i];
            if (!e.joint || e.joint->broken) continue;
            auto self = bodies.find(e.id);
            if (self == bodies.end()) continue;
            const Joint& j = *e.joint;
            JPH::BodyID otherId;   // invalid: the world
            bool otherDynamic = false;
            Vec3 otherOrigin = transformPoint(worlds[i], j.anchor);
            if (!j.connectedTo.empty()) {
                EntityId other = resolve(scene, e, j.connectedTo);
                auto ob = other != kNoEntity ? bodies.find(other) : bodies.end();
                if (ob == bodies.end()) continue;   // (not there, or without a body: nothing to connect to)
                otherId = ob->second.id;
                otherDynamic = ob->second.dynamic;
                int oi = scene.indexOf(other);
                if (oi >= 0) otherOrigin = {worlds[(size_t)oi].m[12], worlds[(size_t)oi].m[13], worlds[(size_t)oi].m[14]};
                if (!j.collideConnected) apart.emplace_back(self->second.id, otherId);
            }
            if (!self->second.dynamic && !otherDynamic) continue;   // nothing moves: nothing to hold
            uint64_t key = 1469598103934665603ull;
            auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
            auto mixf = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); mix(u); };
            mix((uint64_t)j.type);
            mix(self->second.id.GetIndexAndSequenceNumber());
            mix(otherId.GetIndexAndSequenceNumber());
            mix(self->second.dynamic * 2 + otherDynamic);
            for (float f : {j.anchor.x, j.anchor.y, j.anchor.z, j.axis.x, j.axis.y, j.axis.z, j.minLimit, j.maxLimit,
                            j.coneAngle, j.motorStrength})
                mixf(f);
            seen.push_back(e.id);
            auto it = joints.find(e.id);
            if (it != joints.end() && it->second.key == key) {
                if (j.motorStrength > 0.f) applyMotor(it->second, j);   // (the speed may change while playing)
                continue;
            }
            if (it != joints.end()) {
                system.RemoveConstraint(it->second.c);
                joints.erase(it);
            }
            JPH::Ref<JPH::TwoBodyConstraintSettings> settings = jointSettings(j, worlds[i], otherOrigin);
            if (!settings) continue;
            // (the other body first: angles, limits and motors are then this entity's, relative to it)
            JPH::Ref<JPH::TwoBodyConstraint> c = bi().CreateConstraint(settings, otherId, self->second.id);
            if (!c) continue;
            system.AddConstraint(c);
            JointState st{c, key, j.type, j.breakForce};
            applyMotor(st, j);
            joints.emplace(e.id, st);
            bi().ActivateConstraint(c);
        }
        for (auto it = joints.begin(); it != joints.end();) {   // joints removed (or broken, or unconnected)
            if (std::find(seen.begin(), seen.end(), it->first) != seen.end()) { ++it; continue; }
            system.RemoveConstraint(it->second.c);
            it = joints.erase(it);
        }
        // Jointed pairs that mustn't collide: one group table for them all, made again when they change
        uint64_t key = 7;
        for (const auto& [a, b] : apart) key = key * 1000003u + a.GetIndexAndSequenceNumber() * 31u + b.GetIndexAndSequenceNumber();
        if (key == groupsKey) return;
        groupsKey = key;
        for (JPH::BodyID id : grouped) bi().SetCollisionGroup(id, JPH::CollisionGroup());
        grouped.clear();
        if (apart.empty()) { groups = nullptr; return; }
        std::unordered_map<uint32_t, JPH::CollisionGroup::SubGroupID> sub;
        for (const auto& [a, b] : apart)
            for (JPH::BodyID id : {a, b})
                if (!sub.count(id.GetIndexAndSequenceNumber())) {
                    JPH::CollisionGroup::SubGroupID n = (JPH::CollisionGroup::SubGroupID)sub.size();
                    sub[id.GetIndexAndSequenceNumber()] = n;
                    grouped.push_back(id);
                }
        groups = new JPH::GroupFilterTable((JPH::uint)sub.size());
        for (const auto& [a, b] : apart)
            groups->DisableCollision(sub[a.GetIndexAndSequenceNumber()], sub[b.GetIndexAndSequenceNumber()]);
        for (JPH::BodyID id : grouped)
            bi().SetCollisionGroup(id, JPH::CollisionGroup(groups, 0, sub[id.GetIndexAndSequenceNumber()]));
    }

    static void applyMotor(JointState& st, const Joint& j) {
        if (j.motorStrength <= 0.f) return;
        if (st.type == Joint::Type::Hinge) {
            auto* h = static_cast<JPH::HingeConstraint*>(st.c.GetPtr());
            h->SetMotorState(JPH::EMotorState::Velocity);
            h->SetTargetAngularVelocity(j.motorSpeed * DEG2RAD);
        } else if (st.type == Joint::Type::Slider) {
            auto* s = static_cast<JPH::SliderConstraint*>(st.c.GetPtr());
            s->SetMotorState(JPH::EMotorState::Velocity);
            s->SetTargetVelocity(j.motorSpeed);
        }
    }

    // The force holding the joint together in the last step, N
    static float jointForce(const JointState& st, float dt) {
        float impulse = 0.f;
        JPH::TwoBodyConstraint* c = st.c.GetPtr();
        switch (st.type) {
        case Joint::Type::Fixed: impulse = static_cast<JPH::FixedConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
        case Joint::Type::Ball: impulse = static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
        case Joint::Type::Hinge: impulse = static_cast<JPH::HingeConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
        case Joint::Type::Slider: {
            JPH::Vector<2> l = static_cast<JPH::SliderConstraint*>(c)->GetTotalLambdaPosition();
            impulse = std::sqrt(l[0] * l[0] + l[1] * l[1]);
            break;
        }
        case Joint::Type::Distance: impulse = std::fabs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition()); break;
        case Joint::Type::Cone: impulse = static_cast<JPH::SwingTwistConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
        }
        return dt > 0.f ? impulse / dt : 0.f;
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
        uint64_t h = shapeHash(key);
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
                                    objectLayer(e.layer, dynamic));
        t.layer = e.layer;
        s.mUserData = e.id;
        s.mAllowDynamicOrKinematic = !dynamic;   // a static one becomes kinematic once game code moves it
        s.mIsSensor = e.collider->trigger;      // (overlaps are found by triggerOverlaps(), not contacts)
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

Physics::Physics() {
    initJolt();
    impl_ = std::make_unique<Impl>();
}

Physics::~Physics() {
    if (impl_) clear();
}

void Physics::swap(Physics& other) { impl_.swap(other.impl_); }

void Physics::remapMeshes(const std::vector<int>& map) {
    auto remap = [&](ShapeKey& k) {
        if (k.mesh >= 0 && k.mesh < (int)map.size()) k.mesh = map[(size_t)k.mesh];
    };
    for (auto& [id, t] : impl_->bodies) remap(t.key);
    impl_->shapeIndex.clear();
    for (size_t i = 0; i < impl_->shapeCache.size(); ++i) {
        remap(impl_->shapeCache[i].first);
        impl_->shapeIndex[shapeHash(impl_->shapeCache[i].first)].push_back(i);
    }
}

size_t Physics::bodyCount() const { return impl_->bodies.size(); }

void Physics::clear() {
    for (auto& [id, j] : impl_->joints) impl_->system.RemoveConstraint(j.c);
    impl_->joints.clear();
    impl_->groups = nullptr;
    impl_->groupsKey = 0;
    impl_->grouped.clear();
    impl_->characters.clear();   // (their inner bodies go with them)
    for (auto& [id, t] : impl_->bodies) impl_->remove(t);
    impl_->bodies.clear();
    impl_->shapeCache.clear();
    impl_->shapeIndex.clear();
}

std::vector<Impact> Physics::step(Scene& scene, const MeshLibrary& meshes, float dt, const DynamicsSettings& settings) {
    Impl& im = *impl_;
    im.system.SetGravity(toJ(settings.gravity));
    for (int i = 0; i < 32; ++i) im.objectPairs.collides[i] = scene.layers.collides[i];
    std::vector<Mat4> worlds = scene.worldMatrices();
    for (auto& [id, t] : im.bodies) t.seen = false;

    // ---- What the scene looks like now: new, changed and moved bodies
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.collider) continue;
        if (e.collider->shape == Collider::Shape::Mesh && !e.meshRenderer) continue;
        if (auto c = im.characters.find(e.id); c != im.characters.end()) {   // characters move themselves
            c->second.seen = true;
            continue;
        }
        Placement p = place(worlds[i]);
        ShapeKey key;
        key.shape = (int)e.collider->shape;
        key.mesh = e.collider->shape == Collider::Shape::Mesh ? e.meshRenderer->mesh : e.collider->trigger ? -2 : -1;
        key.dynamic = e.rigidBody.has_value() && !e.rigidBody->kinematic && !e.collider->trigger;
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
        if (t.layer != e.layer) {   // moved to another layer
            t.layer = e.layer;
            im.bi().SetObjectLayer(t.id, objectLayer(e.layer, t.dynamic || t.kinematic));
        }
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
                im.bi().SetObjectLayer(t.id, objectLayer(t.layer, true));
                im.bi().SetMotionType(t.id, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
                t.kinematic = true;
            }
            if (dt > 0.f) im.bi().MoveKinematic(t.id, JPH::RVec3(p.position.x, p.position.y, p.position.z), toJ(p.rotation), dt);
            else im.bi().SetPositionAndRotation(t.id, JPH::RVec3(p.position.x, p.position.y, p.position.z), toJ(p.rotation),
                                                JPH::EActivation::Activate);
            t.position = p.position;
            t.rotation = p.rotation;
            t.moving = moved;
        }
    }
    for (auto it = im.characters.begin(); it != im.characters.end();) {   // characters whose entity is gone
        if (it->second.seen) { it->second.seen = false; ++it; }
        else it = im.characters.erase(it);
    }
    for (auto it = im.bodies.begin(); it != im.bodies.end();) {   // entities (or colliders) that are gone
        if (it->second.seen) { ++it; continue; }
        im.remove(it->second);
        it = im.bodies.erase(it);
    }

    im.syncJoints(scene, worlds);
    if (im.added > 256) {   // many new bodies (a scene starting): rebuild the broad-phase's trees once
        im.system.OptimizeBroadPhase();
        im.added = 0;
    }

    // ---- Simulate
    if (dt <= 0.f) return {};   // (just bringing the bodies up to date)
    im.contacts.impacts.clear();
    im.system.Update(dt, 1, &im.temp, &im.jobs);

    // ---- Write the moving bodies back: ancestors first, each against its parent's world as it is now (a chain of
    // bodies, like a ragdoll's bones, moves together)
    std::vector<std::pair<int, int>> order;   // (depth, scene index)
    for (auto& [id, t] : im.bodies) {
        if (!t.dynamic) continue;
        int index = scene.indexOf(id);
        if (index < 0) continue;
        int depth = 0;
        for (EntityId p = scene.entities[(size_t)index].parent; p != kNoEntity && depth < 256; ++depth) {
            const Entity* pe = scene.find(p);
            p = pe ? pe->parent : kNoEntity;
        }
        order.emplace_back(depth, index);
    }
    std::sort(order.begin(), order.end());
    for (const auto& [depth, index] : order) {
        Entity& e = scene.entities[(size_t)index];
        Impl::Tracked& t = im.bodies.find(e.id)->second;
        if (depth > 0) {   // its parents may have just moved
            Mat4 parentWorld = scene.worldMatrix(e.parent);
            worlds[(size_t)scene.indexOf(e.parent)] = parentWorld;
        }
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

    // ---- Joints pulled harder than they hold come apart
    for (auto it = im.joints.begin(); it != im.joints.end();) {
        if (it->second.breakForce > 0.f && Impl::jointForce(it->second, dt) > it->second.breakForce) {
            if (Entity* e = scene.find(it->first); e && e->joint) e->joint->broken = true;
            im.system.RemoveConstraint(it->second.c);
            it = im.joints.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<Impact> impacts;
    for (auto& [id, i] : im.contacts.impacts) impacts.push_back(i);
    return impacts;
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------
bool Physics::raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore, uint32_t layers) const {
    float len = length(dir);
    if (len < 1e-9f || maxDistance <= 0.f) return false;
    dir = dir * (1.f / len);
    JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), toJ(dir * maxDistance)};
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;   // meshes are double-sided
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    QueryFilter filter(false, ignore, layers);
    impl_->system.GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, {}, {}, filter);
    if (!collector.HadHit()) return false;
    const JPH::RayCastResult& r = collector.mHit;
    JPH::RVec3 p = ray.GetPointOnRay(r.mFraction);
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), r.mBodyID);
    if (!lock.Succeeded()) return false;
    const JPH::Body& body = lock.GetBody();
    Vec3 n = fromJ(body.GetWorldSpaceSurfaceNormal(r.mSubShapeID2, p));
    if (dot(n, dir) > 0.f) n = n * -1.f;   // (a back face)
    hit = {(EntityId)body.GetUserData(), {(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()}, n, r.mFraction * maxDistance};
    return true;
}

std::vector<RaycastHit> Physics::raycastAll(Vec3 origin, Vec3 dir, float maxDistance, uint32_t layers) const {
    std::vector<RaycastHit> hits;
    float len = length(dir);
    if (len < 1e-9f || maxDistance <= 0.f) return hits;
    dir = dir * (1.f / len);
    JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z), toJ(dir * maxDistance)};
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    QueryFilter filter(false, kNoEntity, layers);
    impl_->system.GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, {}, {}, filter);
    collector.Sort();
    for (const JPH::RayCastResult& r : collector.mHits) {
        JPH::RVec3 p = ray.GetPointOnRay(r.mFraction);
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), r.mBodyID);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        Vec3 n = fromJ(body.GetWorldSpaceSurfaceNormal(r.mSubShapeID2, p));
        if (dot(n, dir) > 0.f) continue;   // the inside of a convex body the ray started in
        hits.push_back({(EntityId)body.GetUserData(), {(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()}, n,
                        r.mFraction * maxDistance});
    }
    return hits;
}

bool Physics::bounds(Vec3& min, Vec3& max) const {
    JPH::AABox box;   // (the broad phase's own bounds aren't tight)
    for (const auto& [entity, tracked] : impl_->bodies) {
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), tracked.id);
        if (lock.Succeeded()) box.Encapsulate(lock.GetBody().GetWorldSpaceBounds());
    }
    if (!box.IsValid()) return false;
    min = {(float)box.mMin.GetX(), (float)box.mMin.GetY(), (float)box.mMin.GetZ()};
    max = {(float)box.mMax.GetX(), (float)box.mMax.GetY(), (float)box.mMax.GetZ()};
    return true;
}

std::vector<EntityId> Physics::overlapSphere(Vec3 center, float radius, bool includeTriggers, EntityId ignore,
                                            uint32_t layers) const {
    std::vector<EntityId> out;
    if (radius <= 0.f) return out;
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryFilter filter(includeTriggers, ignore, layers);
    impl_->system.GetNarrowPhaseQueryNoLock().CollideShape(&sphere, JPH::Vec3::sReplicate(1.f),
                                                          JPH::RMat44::sTranslation(JPH::RVec3(center.x, center.y, center.z)),
                                                          settings, JPH::RVec3::sZero(), collector, {}, {}, filter);
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), r.mBodyID2);
        if (!lock.Succeeded()) continue;
        EntityId id = (EntityId)lock.GetBody().GetUserData();
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

std::vector<std::pair<EntityId, EntityId>> Physics::triggerOverlaps(const Scene& scene) const {
    std::vector<std::pair<EntityId, EntityId>> out;
    for (const Entity& e : scene.entities) {
        if (!e.collider || !e.collider->trigger) continue;
        auto it = impl_->bodies.find(e.id);
        if (it == impl_->bodies.end()) continue;
        JPH::RefConst<JPH::Shape> shape;
        JPH::RMat44 transform;
        {
            JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), it->second.id);
            if (!lock.Succeeded()) continue;
            shape = lock.GetBody().GetShape();
            transform = lock.GetBody().GetCenterOfMassTransform();
        }
        JPH::CollideShapeSettings settings;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        QueryFilter filter(false, e.id, impl_->objectPairs.collides[e.layer & 31]);   // the layers it meets
        impl_->system.GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sReplicate(1.f), transform, settings,
                                                              JPH::RVec3::sZero(), collector, {}, {}, filter);
        std::vector<EntityId> others;
        for (const JPH::CollideShapeResult& r : collector.mHits) {
            JPH::BodyLockRead lock(impl_->system.GetBodyLockInterfaceNoLock(), r.mBodyID2);
            if (!lock.Succeeded()) continue;
            EntityId id = (EntityId)lock.GetBody().GetUserData();
            if (std::find(others.begin(), others.end(), id) == others.end()) others.push_back(id);
        }
        std::sort(others.begin(), others.end(), [&](EntityId a, EntityId b) { return scene.indexOf(a) < scene.indexOf(b); });
        for (EntityId o : others) out.emplace_back(e.id, o);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Characters
// ---------------------------------------------------------------------------------------------
bool Physics::isCharacter(EntityId id) const { return impl_->characters.count(id) > 0; }

CharacterMove Physics::moveCharacter(Scene& scene, const MeshLibrary& meshes, Entity& self, Vec3 delta, float dt) {
    Impl& im = *impl_;
    CharacterMove result;
    result.displacement = delta;
    Mat4 world = scene.worldMatrix(self.id);
    Placement p = place(world);
    bool shaped = self.collider && !self.collider->trigger &&
                  (self.collider->shape == Collider::Shape::Capsule || self.collider->shape == Collider::Shape::Sphere);
    if (shaped) {
        ShapeKey key;
        key.shape = (int)self.collider->shape;
        const Collider& c = *self.collider;
        const float values[9] = {p.scale.x, p.scale.y, p.scale.z, c.center.x, c.center.y, c.center.z,
                                 (float)self.layer, c.radius, c.height};   // (a new layer: made again)
        std::memcpy(key.values, values, sizeof(values));
        auto it = im.characters.find(self.id);
        if (it == im.characters.end() || !(it->second.key == key)) {
            if (auto b = im.bodies.find(self.id); b != im.bodies.end()) {   // it was an ordinary body until now
                im.remove(b->second);
                im.bodies.erase(b);
            }
            JPH::RefConst<JPH::Shape> shape = im.makeShape(self, meshes, p.scale, false);
            if (shape) {
                JPH::CharacterVirtualSettings cs;
                cs.mShape = shape;
                cs.mInnerBodyShape = shape;            // a body too, so rays, overlaps and triggers find it
                cs.mInnerBodyLayer = objectLayer(self.layer, true);
                cs.mMaxSlopeAngle = 50.f * DEG2RAD;
                cs.mMass = 70.f;
                cs.mMaxStrength = 100.f;               // how hard it pushes bodies
                cs.mCharacterPadding = 0.02f;
                cs.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
                // Contacts below the centre of its bottom (hemi)sphere hold it up; higher ones only push
                float r = c.radius * std::fmax(p.scale.x, p.scale.z);
                float halfSeg = c.shape == Collider::Shape::Capsule ? std::fmax(c.height * 0.5f * p.scale.y - r, 0.f) : 0.f;
                cs.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -(c.center.y * p.scale.y - halfSeg));
                Impl::Character ch;
                ch.character = new JPH::CharacterVirtual(&cs, JPH::RVec3(p.position.x, p.position.y, p.position.z),
                                                         JPH::Quat::sIdentity(), (JPH::uint64)self.id, &im.system);
                ch.key = key;
                ch.position = p.position;
                it = im.characters.insert_or_assign(self.id, ch).first;
            }
        }
        if (it != im.characters.end()) {
            Impl::Character& ch = it->second;
            ch.seen = true;
            JPH::CharacterVirtual& cv = *ch.character;
            if (!near(p.position, ch.position, 1e-4f))   // game code put it somewhere else (a respawn)
                cv.SetPosition(JPH::RVec3(p.position.x, p.position.y, p.position.z));
            JPH::RVec3 before = cv.GetPosition();
            if (dt > 0.f) {
                cv.UpdateGroundVelocity();
                JPH::Vec3 v = toJ(delta * (1.f / dt));
                if (cv.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround) v += cv.GetGroundVelocity();
                cv.SetLinearVelocity(v);
                JPH::CharacterVirtual::ExtendedUpdateSettings settings;
                settings.mStickToFloorStepDown = JPH::Vec3(0, -0.4f, 0);
                settings.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);
                QueryFilter filter(false, kNoEntity);
                cv.ExtendedUpdate(dt, JPH::Vec3(0, -18.f, 0), settings,
                                  im.system.GetDefaultBroadPhaseLayerFilter(objectLayer(self.layer, true)),
                                  im.system.GetDefaultLayerFilter(objectLayer(self.layer, true)), filter, {}, im.temp);
            } else {
                cv.SetPosition(before + JPH::RVec3(delta.x, delta.y, delta.z));
            }
            JPH::RVec3 after = cv.GetPosition();
            result.displacement = {(float)(after.GetX() - before.GetX()), (float)(after.GetY() - before.GetY()),
                                   (float)(after.GetZ() - before.GetZ())};
            result.grounded = cv.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            for (const JPH::CharacterContact& c : cv.GetActiveContacts()) {
                if (!c.mHadCollision) continue;
                float ny = c.mSurfaceNormal.GetY();
                if (ny < -0.7f) result.hitCeiling = true;
                else if (ny <= 0.7f) result.hitWall = true;
            }
            ch.position = {(float)after.GetX(), (float)after.GetY(), (float)after.GetZ()};
        }
    }
    // World-space displacement -> the entity's parent space
    Vec3 local = transformDir(inverseAffine(scene.parentWorldMatrix(self.id)), result.displacement);
    self.transform.position[0] += local.x;
    self.transform.position[1] += local.y;
    self.transform.position[2] += local.z;
    if (shaped)
        if (auto it = im.characters.find(self.id); it != im.characters.end()) {   // what the entity now says it is
            Mat4 now = scene.worldMatrix(self.id);
            it->second.position = {now.m[12], now.m[13], now.m[14]};
        }
    return result;
}
