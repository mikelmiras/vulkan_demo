#include "scene/scene_diff.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "core/json.h"
#include "scene/serialization.h"

namespace {

bool eq(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool eq(const float* a, const float* b, int n) { return std::memcmp(a, b, sizeof(float) * (size_t)n) == 0; }

bool eq(const MeshRenderer& a, const MeshRenderer& b) {
    return a.mesh == b.mesh && eq(a.color, b.color) && a.texture == b.texture && a.tiling == b.tiling &&
           a.checker == b.checker && a.metallic == b.metallic && a.roughness == b.roughness &&
           a.metalRoughTexture == b.metalRoughTexture && a.normalTexture == b.normalTexture &&
           a.normalScale == b.normalScale && eq(a.emissive, b.emissive) && a.emissiveStrength == b.emissiveStrength &&
           a.emissiveTexture == b.emissiveTexture && a.alphaMode == b.alphaMode && a.opacity == b.opacity &&
           a.alphaCutoff == b.alphaCutoff;
}
bool eq(const Camera& a, const Camera& b) { return a.fovY == b.fovY && a.zNear == b.zNear && a.zFar == b.zFar; }
bool eq(const Collider& a, const Collider& b) {
    return a.shape == b.shape && eq(a.center, b.center) && eq(a.size, b.size) && a.radius == b.radius &&
           a.height == b.height && a.trigger == b.trigger;
}
bool eq(const Light& a, const Light& b) {
    return a.type == b.type && eq(a.color, b.color) && a.intensity == b.intensity && a.range == b.range &&
           a.spotAngle == b.spotAngle && a.spotSoftness == b.spotSoftness && a.castShadows == b.castShadows;
}
bool eq(const RigidBody& a, const RigidBody& b) {
    return a.mass == b.mass && a.lockRotation == b.lockRotation && a.useGravity == b.useGravity &&
           a.bounciness == b.bounciness && a.friction == b.friction && a.damping == b.damping &&
           eq(a.velocity, b.velocity) && eq(a.angularVelocity, b.angularVelocity);
}
bool eq(const AudioSource& a, const AudioSource& b) {
    return a.clip == b.clip && a.volume == b.volume && a.loop == b.loop && a.playOnStart == b.playOnStart &&
           a.playOnImpact == b.playOnImpact && a.spatial == b.spatial && a.minDistance == b.minDistance &&
           a.maxDistance == b.maxDistance;
}
bool eq(const ParticleEmitter& a, const ParticleEmitter& b) {
    return a.rate == b.rate && a.burst == b.burst && a.playOnStart == b.playOnStart && a.duration == b.duration &&
           a.lifetime == b.lifetime && a.lifetimeRandom == b.lifetimeRandom && a.speed == b.speed &&
           a.speedRandom == b.speedRandom && a.spread == b.spread && a.radius == b.radius && a.gravity == b.gravity &&
           a.drag == b.drag && a.startSize == b.startSize && a.endSize == b.endSize && eq(a.startColor, b.startColor) &&
           eq(a.endColor, b.endColor) && a.startAlpha == b.startAlpha && a.endAlpha == b.endAlpha &&
           a.intensity == b.intensity && a.additive == b.additive && a.maxParticles == b.maxParticles;
}
bool eq(const PrefabLink& a, const PrefabLink& b) { return a.prefab == b.prefab && a.source == b.source; }
bool eq(const Skin& a, const Skin& b) {
    if (a.joints != b.joints || a.inverseBind.size() != b.inverseBind.size()) return false;
    for (size_t i = 0; i < a.inverseBind.size(); ++i)
        if (!eq(a.inverseBind[i].m, b.inverseBind[i].m, 16)) return false;
    return true;
}
bool eq(const Animator& a, const Animator& b) {
    return a.clip == b.clip && a.speed == b.speed && a.loop == b.loop && a.playOnStart == b.playOnStart &&
           a.playing == b.playing && a.time == b.time && a.fadeFrom == b.fadeFrom && a.fadeFromTime == b.fadeFromTime &&
           a.fade == b.fade && a.fadeLength == b.fadeLength;
}

template <class T> bool eq(const std::optional<T>& a, const std::optional<T>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || eq(*a, *b);
}

bool eq(const Behaviour& a, const Behaviour& b) {
    if (std::strcmp(a.typeName(), b.typeName()) != 0) return false;
    // Fields and runtime state, through the same visitors that save them
    if (json::write(behaviourToJson(a)) != json::write(behaviourToJson(b))) return false;
    return json::write(behaviourStateToJson(const_cast<Behaviour&>(a))) ==
           json::write(behaviourStateToJson(const_cast<Behaviour&>(b)));
}

bool samePrefabs(const std::vector<Prefab>& a, const std::vector<Prefab>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].entities.size() != b[i].entities.size()) return false;
        for (size_t k = 0; k < a[i].entities.size(); ++k)
            if (!sameEntity(a[i].entities[k], b[i].entities[k])) return false;
    }
    return true;
}

}  // namespace

bool sameEntity(const Entity& a, const Entity& b) {
    if (a.id != b.id || a.name != b.name || a.parent != b.parent || a.visible != b.visible || a.locked != b.locked)
        return false;
    if (!eq(a.transform.position, b.transform.position, 3) || !eq(a.transform.rotation, b.transform.rotation, 3) ||
        !eq(a.transform.scale, b.transform.scale, 3))
        return false;
    if (!eq(a.meshRenderer, b.meshRenderer) || !eq(a.camera, b.camera) || !eq(a.collider, b.collider) ||
        !eq(a.light, b.light) || !eq(a.rigidBody, b.rigidBody) || !eq(a.audioSource, b.audioSource) ||
        !eq(a.prefab, b.prefab) || !eq(a.skin, b.skin) || !eq(a.animator, b.animator) ||
        !eq(a.particleEmitter, b.particleEmitter))
        return false;
    if (a.behaviours.size() != b.behaviours.size()) return false;
    for (size_t i = 0; i < a.behaviours.size(); ++i) {
        const Behaviour* x = a.behaviours[i].get();
        const Behaviour* y = b.behaviours[i].get();
        if (!x || !y) {
            if (x != y) return false;
            continue;
        }
        if (!eq(*x, *y)) return false;
    }
    return true;
}

bool sameInputActions(const std::vector<InputAction>& a, const std::vector<InputAction>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].keys != b[i].keys || a[i].mouseButton != b[i].mouseButton) return false;
    return true;
}

bool sameRenderSettings(const RenderSettings& a, const RenderSettings& b) {
    return eq(a.sky, b.sky) && eq(a.zenith, b.zenith) && eq(a.ground, b.ground) && a.fogDensity == b.fogDensity && a.ambient == b.ambient && a.exposure == b.exposure &&
           a.bloomStrength == b.bloomStrength && a.bloomThreshold == b.bloomThreshold;
}

SceneDelta diffScenes(const Scene& before, const Scene& after) {
    SceneDelta d;
    std::unordered_map<EntityId, size_t> inAfter;
    inAfter.reserve(after.entities.size());
    for (size_t i = 0; i < after.entities.size(); ++i) inAfter.emplace(after.entities[i].id, i);
    std::unordered_set<EntityId> inBefore;
    inBefore.reserve(before.entities.size());
    std::vector<EntityId> beforeOrder, afterOrder;   // the entities both have, in each one's order
    for (size_t i = 0; i < before.entities.size(); ++i) {
        const Entity& e = before.entities[i];
        inBefore.insert(e.id);
        auto it = inAfter.find(e.id);
        if (it == inAfter.end()) {
            d.entities.emplace_back(i, e);   // removed since
            continue;
        }
        beforeOrder.push_back(e.id);
        if (!sameEntity(e, after.entities[it->second])) d.entities.emplace_back(i, e);
    }
    for (const Entity& e : after.entities) {
        if (!inBefore.count(e.id)) d.added.push_back(e.id);
        else afterOrder.push_back(e.id);
    }
    if (beforeOrder != afterOrder) {
        std::vector<EntityId> order;
        for (const Entity& e : before.entities) order.push_back(e.id);
        d.order = std::move(order);
    }
    if (!samePrefabs(before.prefabs, after.prefabs)) d.prefabs = before.prefabs;
    if (!sameInputActions(before.inputActions, after.inputActions)) d.inputActions = before.inputActions;
    if (!sameRenderSettings(before.render, after.render)) d.render = before.render;
    return d;
}

void applyDelta(Scene& scene, const SceneDelta& d) {
    if (!d.added.empty()) {   // (not Scene::remove: that would take their children, which may be older entities)
        std::unordered_set<EntityId> gone(d.added.begin(), d.added.end());
        scene.entities.erase(std::remove_if(scene.entities.begin(), scene.entities.end(),
                                            [&](const Entity& e) { return gone.count(e.id) > 0; }),
                             scene.entities.end());
    }
    // Changed ones are replaced in place; removed ones go back where they were (in index order, so earlier
    // insertions don't shift later ones)
    std::vector<const std::pair<size_t, Entity>*> restore;
    for (const auto& item : d.entities) restore.push_back(&item);
    std::sort(restore.begin(), restore.end(), [](auto* a, auto* b) { return a->first < b->first; });
    for (const auto* item : restore) {
        if (Entity* e = scene.find(item->second.id)) *e = item->second;
        else scene.entities.insert(scene.entities.begin() + (std::ptrdiff_t)std::min(item->first, scene.entities.size()), item->second);
    }
    if (d.order) {
        std::unordered_map<EntityId, size_t> rank;
        for (size_t i = 0; i < d.order->size(); ++i) rank.emplace((*d.order)[i], i);
        std::stable_sort(scene.entities.begin(), scene.entities.end(), [&](const Entity& a, const Entity& b) {
            auto ra = rank.find(a.id), rb = rank.find(b.id);
            size_t x = ra != rank.end() ? ra->second : SIZE_MAX, y = rb != rank.end() ? rb->second : SIZE_MAX;
            return x < y;
        });
    }
    if (d.prefabs) scene.prefabs = *d.prefabs;
    if (d.inputActions) scene.inputActions = *d.inputActions;
    if (d.render) scene.render = *d.render;
}
