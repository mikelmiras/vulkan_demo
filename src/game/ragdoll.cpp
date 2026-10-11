#include "game/ragdoll.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

// The entity and everything under it
std::vector<EntityId> subtree(const Scene& scene, EntityId root) {
    std::vector<EntityId> out{root};
    for (size_t i = 0; i < out.size(); ++i)
        for (const Entity& e : scene.entities)
            if (e.parent == out[i]) out.push_back(e.id);
    return out;
}

}  // namespace

int makeRagdoll(Scene& scene, EntityId root, float totalMass) {
    std::unordered_map<std::string, EntityId> names = scene.namesUnder(root);
    std::unordered_set<EntityId> bones;
    for (EntityId id : subtree(scene, root))
        if (const Entity* e = scene.find(id); e && e->skin)
            for (const std::string& j : e->skin->joints)
                if (auto it = names.find(j); it != names.end()) bones.insert(it->second);
    if (bones.empty()) return 0;

    // Each bone reaches to its first child bone (leaves: half their parent's length)
    std::unordered_map<EntityId, Vec3> reach;
    for (EntityId b : bones)
        for (const Entity& c : scene.entities)
            if (c.parent == b && bones.count(c.id) && !reach.count(b)) reach[b] = c.transform.pos();
    float total = 0.f;
    std::vector<EntityId> ordered;
    for (const Entity& e : scene.entities)
        if (bones.count(e.id)) ordered.push_back(e.id);   // scene order: parents first
    for (EntityId b : ordered)
        if (!reach.count(b)) {
            const Entity* e = scene.find(b);
            float l = e && reach.count(e->parent) ? length(reach[e->parent]) * 0.5f : 0.1f;
            reach[b] = {0.f, std::max(l, 0.02f), 0.f};
        }
    for (EntityId b : ordered) total += length(reach[b]);

    for (EntityId b : ordered) {
        Entity& e = *scene.find(b);
        Vec3 r = reach[b];
        float len = std::max(length(r), 1e-3f);
        if (!e.collider) {
            Collider c;
            if (std::fabs(r.y) > 0.7f * len) {   // along the bone
                c.shape = Collider::Shape::Capsule;
                c.radius = std::clamp(len * 0.18f, 0.01f, len * 0.45f);
                c.height = std::max(std::fabs(r.y), 2.f * c.radius + 1e-3f);
                c.center = {0.f, r.y * 0.5f, 0.f};
            } else {
                c.shape = Collider::Shape::Sphere;
                c.radius = len * 0.3f;
                c.center = r * 0.5f;
            }
            e.collider = c;
        }
        RigidBody rb;
        rb.mass = std::max(totalMass * len / std::max(total, 1e-6f), 0.1f);
        rb.kinematic = true;
        rb.damping = 0.1f;
        e.rigidBody = rb;
        if (const Entity* parent = scene.find(e.parent); parent && bones.count(parent->id)) {
            Joint j;
            j.type = Joint::Type::Cone;
            j.connectedTo = parent->name;
            j.axis = {0.f, 1.f, 0.f};
            j.coneAngle = 40.f;
            e.joint = j;
        }
    }
    return (int)ordered.size();
}

void setRagdollLimp(Scene& scene, EntityId root, bool limp) {
    for (EntityId id : subtree(scene, root)) {
        Entity* e = scene.find(id);
        if (!e) continue;
        if (e->rigidBody && e->rigidBody->kinematic == limp) {
            e->rigidBody->kinematic = !limp;
            e->rigidBody->velocity = {};
            e->rigidBody->angularVelocity = {};
        }
        if (limp && e->animator) e->animator->stop();
    }
}
