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
           a.alphaCutoff == b.alphaCutoff && a.clearcoat == b.clearcoat && a.clearcoatRoughness == b.clearcoatRoughness &&
           a.sheen == b.sheen && a.anisotropy == b.anisotropy && a.heightTexture == b.heightTexture &&
           a.parallaxDepth == b.parallaxDepth && a.detailTexture == b.detailTexture && a.detailTiling == b.detailTiling &&
           a.detailStrength == b.detailStrength && a.sheltered == b.sheltered && a.translucency == b.translucency &&
           a.subsurface == b.subsurface && eq(a.subsurfaceColor, b.subsurfaceColor);
}
bool eq(const Camera& a, const Camera& b) {
    return a.fovY == b.fovY && a.zNear == b.zNear && a.zFar == b.zFar && a.player == b.player;
}
bool eq(const Collider& a, const Collider& b) {
    return a.shape == b.shape && eq(a.center, b.center) && eq(a.size, b.size) && a.radius == b.radius &&
           a.height == b.height && a.trigger == b.trigger;
}
bool eq(const Light& a, const Light& b) {
    return a.type == b.type && eq(a.color, b.color) && a.intensity == b.intensity && a.range == b.range &&
           a.spotAngle == b.spotAngle && a.spotSoftness == b.spotSoftness && a.size == b.size && a.length == b.length && a.height == b.height && a.castShadows == b.castShadows &&
           a.cookie == b.cookie;
}
bool eq(const RigidBody& a, const RigidBody& b) {
    return a.mass == b.mass && a.lockRotation == b.lockRotation && a.useGravity == b.useGravity &&
           a.bounciness == b.bounciness && a.friction == b.friction && a.damping == b.damping &&
           eq(a.velocity, b.velocity) && eq(a.angularVelocity, b.angularVelocity) && a.kinematic == b.kinematic &&
           a.buoyancy == b.buoyancy;
}
bool eq(const NavLink& a, const NavLink& b) { return eq(a.end, b.end) && a.bothWays == b.bothWays && a.cost == b.cost; }
bool eq(const Joint& a, const Joint& b) {
    return a.type == b.type && a.connectedTo == b.connectedTo && eq(a.anchor, b.anchor) && eq(a.axis, b.axis) &&
           a.minLimit == b.minLimit && a.maxLimit == b.maxLimit && a.coneAngle == b.coneAngle &&
           a.motorSpeed == b.motorSpeed && a.motorStrength == b.motorStrength && a.breakForce == b.breakForce &&
           a.collideConnected == b.collideConnected && a.broken == b.broken;
}
bool eq(const AudioSource& a, const AudioSource& b) {
    return a.clip == b.clip && a.volume == b.volume && a.loop == b.loop && a.playOnStart == b.playOnStart &&
           a.playOnImpact == b.playOnImpact && a.spatial == b.spatial && a.minDistance == b.minDistance &&
           a.maxDistance == b.maxDistance && a.bus == b.bus && a.stream == b.stream && a.doppler == b.doppler &&
           a.occlusion == b.occlusion;
}
bool eq(const ReverbZone& a, const ReverbZone& b) {
    return a.radius == b.radius && a.blend == b.blend && a.wet == b.wet && a.roomSize == b.roomSize && a.damping == b.damping;
}
bool eq(const SoftBody& a, const SoftBody& b) {
    return a.shape == b.shape && a.width == b.width && a.height == b.height && a.resolution == b.resolution &&
           a.pinned == b.pinned && a.mass == b.mass && a.stiffness == b.stiffness && a.bendiness == b.bendiness &&
           a.pressure == b.pressure && a.damping == b.damping && a.friction == b.friction &&
           a.tearStretch == b.tearStretch && a.buoyancy == b.buoyancy;
}
bool eq(const Fracture& a, const Fracture& b) {
    return a.pieces == b.pieces && a.seed == b.seed && a.breakSpeed == b.breakSpeed && a.burst == b.burst &&
           a.pieceLife == b.pieceLife && a.holdTogether == b.holdTogether && a.holdStrength == b.holdStrength;
}
bool eq(const AIBrain& a, const AIBrain& b) {
    if (a.nodes.size() != b.nodes.size()) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const AIBrain::Node &x = a.nodes[i], &y = b.nodes[i];
        if (x.type != y.type || x.name != y.name || x.children != y.children || x.key != y.key || x.text != y.text ||
            x.number != y.number || x.number2 != y.number2)
            return false;
    }
    return json::write(a.blackboard) == json::write(b.blackboard) && a.perceives == b.perceives &&
           a.sightRange == b.sightRange && a.sightAngle == b.sightAngle && a.eyeHeight == b.eyeHeight &&
           a.hearingRange == b.hearingRange && a.memory == b.memory;
}
bool eq(const Vehicle& a, const Vehicle& b) {
    if (a.wheels.size() != b.wheels.size()) return false;
    for (size_t i = 0; i < a.wheels.size(); ++i) {
        const Vehicle::Wheel &x = a.wheels[i], &y = b.wheels[i];
        if (!eq(x.position, y.position) || x.radius != y.radius || x.width != y.width || x.steering != y.steering ||
            x.driven != y.driven || x.entity != y.entity)
            return false;
    }
    return a.engineTorque == b.engineTorque && a.maxRpm == b.maxRpm && a.maxSteer == b.maxSteer &&
           a.suspensionLength == b.suspensionLength && a.suspensionFrequency == b.suspensionFrequency &&
           a.suspensionDamping == b.suspensionDamping;
}
bool eq(const UIElement& a, const UIElement& b) {
    return a.kind == b.kind && eq(a.anchorMin, b.anchorMin, 2) && eq(a.anchorMax, b.anchorMax, 2) &&
           eq(a.offsetMin, b.offsetMin, 2) && eq(a.offsetMax, b.offsetMax, 2) && eq(a.color, b.color) &&
           a.opacity == b.opacity && a.rounding == b.rounding && a.text == b.text && a.textSize == b.textSize &&
           eq(a.textColor, b.textColor) && a.align == b.align && a.font == b.font && a.image == b.image &&
           a.action == b.action && a.target == b.target && a.value == b.value && a.min == b.min && a.max == b.max &&
           a.spacing == b.spacing;
}
bool eq(const Water& a, const Water& b) {   // (its mesh: runtime)
    return a.width == b.width && a.length == b.length && a.depth == b.depth && a.waveHeight == b.waveHeight &&
           a.waveLength == b.waveLength && a.waveDirection == b.waveDirection && a.choppiness == b.choppiness &&
           eq(a.current, b.current) && eq(a.color, b.color) && a.clarity == b.clarity;
}
bool eq(const Decal& a, const Decal& b) {
    return a.texture == b.texture && eq(a.color, b.color) && a.opacity == b.opacity && a.roughness == b.roughness &&
           a.wetness == b.wetness && a.angleFade == b.angleFade && a.normalTexture == b.normalTexture &&
           a.normalStrength == b.normalStrength && a.emissive == b.emissive;
}
bool eq(const ReflectionProbe& a, const ReflectionProbe& b) {
    return eq(a.size, b.size) && a.resolution == b.resolution && a.boxProjection == b.boxProjection;
}
// (any change to the heights or paint gives a terrain a new revision: no need to compare them)
bool eq(const Terrain& a, const Terrain& b) {
    if (a.revision != b.revision || a.resolution != b.resolution || a.size != b.size) return false;
    for (int k = 0; k < Terrain::kLayers; ++k)
        if (a.layers[k].texture != b.layers[k].texture || !eq(a.layers[k].color, b.layers[k].color) ||
            a.layers[k].tiling != b.layers[k].tiling || a.layers[k].normalTexture != b.layers[k].normalTexture ||
            a.layers[k].normalStrength != b.layers[k].normalStrength)
            return false;
    if (a.foliage.size() != b.foliage.size()) return false;   // (holes and painting: a new revision)
    for (size_t k = 0; k < a.foliage.size(); ++k) {
        const Terrain::Foliage &x = a.foliage[k], &y = b.foliage[k];
        if (x.mesh != y.mesh || !eq(x.color, y.color) || x.density != y.density || x.minScale != y.minScale ||
            x.maxScale != y.maxScale || x.wind != y.wind || x.fadeDistance != y.fadeDistance)
            return false;
    }
    return true;
}
bool eq(const ParticleEmitter& a, const ParticleEmitter& b) {
    return a.rate == b.rate && a.burst == b.burst && a.playOnStart == b.playOnStart && a.duration == b.duration &&
           a.lifetime == b.lifetime && a.lifetimeRandom == b.lifetimeRandom && a.speed == b.speed &&
           a.speedRandom == b.speedRandom && a.spread == b.spread && a.radius == b.radius && a.gravity == b.gravity &&
           a.drag == b.drag && a.startSize == b.startSize && a.endSize == b.endSize && eq(a.startColor, b.startColor) &&
           eq(a.endColor, b.endColor) && a.startAlpha == b.startAlpha && a.endAlpha == b.endAlpha &&
           a.intensity == b.intensity && a.additive == b.additive && a.maxParticles == b.maxParticles &&
           a.stretch == b.stretch && a.lit == b.lit && a.gpu == b.gpu;
}
bool eq(const PrefabLink& a, const PrefabLink& b) { return a.prefab == b.prefab && a.source == b.source; }
bool eq(const Skin& a, const Skin& b) {
    if (a.joints != b.joints || a.inverseBind.size() != b.inverseBind.size()) return false;
    for (size_t i = 0; i < a.inverseBind.size(); ++i)
        if (!eq(a.inverseBind[i].m, b.inverseBind[i].m, 16)) return false;
    return true;
}
bool eq(const AnimationEvent& a, const AnimationEvent& b) {
    return a.clip == b.clip && a.time == b.time && a.name == b.name && a.sound == b.sound && a.bone == b.bone &&
           a.volume == b.volume && a.particles == b.particles && a.shake == b.shake;
}
bool eq(const BlendClip& a, const BlendClip& b) { return a.clip == b.clip && a.at == b.at && a.atY == b.atY; }
bool eq(const AnimatorIK& a, const AnimatorIK& b) {
    return a.name == b.name && a.type == b.type && a.bone == b.bone && a.targetEntity == b.targetEntity &&
           eq(a.pole, b.pole) && eq(a.aimAxis, b.aimAxis) && a.maxAngle == b.maxAngle && a.weight == b.weight &&
           eq(a.target, b.target) && a.hasTarget == b.hasTarget;
}
bool eq(const AnimatorLayer& a, const AnimatorLayer& b) {
    return a.name == b.name && a.clip == b.clip && a.mask == b.mask && a.weight == b.weight && a.speed == b.speed &&
           a.loop == b.loop && a.additive == b.additive && a.playing == b.playing && a.time == b.time;
}
template <class T> bool eqList(const std::vector<T>& a, const std::vector<T>& b);
bool eq(const AnimatorState& a, const AnimatorState& b) {
    return a.name == b.name && a.clip == b.clip && a.speed == b.speed && a.loop == b.loop &&
           a.blendParameter == b.blendParameter && a.blendParameterY == b.blendParameterY && eqList(a.blend, b.blend);
}
bool eq(const AnimatorTransition& a, const AnimatorTransition& b) {
    return a.from == b.from && a.to == b.to && a.parameter == b.parameter && a.when == b.when && a.value == b.value &&
           a.fade == b.fade;
}
template <class T> bool eqList(const std::vector<T>& a, const std::vector<T>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!eq(a[i], b[i])) return false;
    return true;
}
bool eq(const Animator& a, const Animator& b) {
    return eqList(a.layers, b.layers) && eqList(a.ik, b.ik) && a.rootMotion == b.rootMotion && a.rootBone == b.rootBone &&
           a.fadeFromState == b.fadeFromState && eqList(a.events, b.events) && eqList(a.states, b.states) && eqList(a.transitions, b.transitions) &&
           a.startState == b.startState && a.state == b.state && a.parameters == b.parameters && a.clip == b.clip && a.speed == b.speed && a.loop == b.loop && a.playOnStart == b.playOnStart &&
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
    if (a.id != b.id || a.name != b.name || a.parent != b.parent || a.visible != b.visible || a.locked != b.locked ||
        a.tags != b.tags || a.layer != b.layer)
        return false;
    if (!eq(a.transform.position, b.transform.position, 3) || !eq(a.transform.rotation, b.transform.rotation, 3) ||
        !eq(a.transform.scale, b.transform.scale, 3))
        return false;
    if (!eq(a.meshRenderer, b.meshRenderer) || !eq(a.camera, b.camera) || !eq(a.collider, b.collider) ||
        !eq(a.light, b.light) || !eq(a.rigidBody, b.rigidBody) || !eq(a.joint, b.joint) || !eq(a.navLink, b.navLink) || !eq(a.audioSource, b.audioSource) || !eq(a.reverbZone, b.reverbZone) ||
        !eq(a.prefab, b.prefab) || !eq(a.nestedPrefab, b.nestedPrefab) || !eq(a.skin, b.skin) || !eq(a.animator, b.animator) ||
        !eq(a.particleEmitter, b.particleEmitter) || !eq(a.terrain, b.terrain) ||
        !eq(a.reflectionProbe, b.reflectionProbe) || !eq(a.decal, b.decal) || !eq(a.water, b.water) || !eq(a.ui, b.ui) || !eq(a.vehicle, b.vehicle) || !eq(a.brain, b.brain) || !eq(a.softBody, b.softBody) || !eq(a.fracture, b.fracture) ||
        a.replicated.has_value() != b.replicated.has_value() || (a.replicated && a.replicated->fields != b.replicated->fields))
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
           a.bloomStrength == b.bloomStrength && a.bloomThreshold == b.bloomThreshold && a.aoStrength == b.aoStrength &&
           a.aoRadius == b.aoRadius && a.skyImage == b.skyImage && a.skyIntensity == b.skyIntensity &&
           a.skyRotation == b.skyRotation && a.gi == b.gi && a.giSpacing == b.giSpacing && a.giIntensity == b.giIntensity &&
           a.reflections == b.reflections && a.shadowSoftness == b.shadowSoftness && a.volumetricFog == b.volumetricFog &&
           a.fogLight == b.fogLight && a.fogAnisotropy == b.fogAnisotropy && a.fogHeightFalloff == b.fogHeightFalloff &&
           a.tonemapper == b.tonemapper && a.contrast == b.contrast && a.saturation == b.saturation && a.vignette == b.vignette &&
           a.autoExposure == b.autoExposure && a.exposureMin == b.exposureMin && a.exposureMax == b.exposureMax &&
           a.adaptationSpeed == b.adaptationSpeed && a.localToneMapping == b.localToneMapping && a.lensDirt == b.lensDirt && a.lensFlare == b.lensFlare &&
           a.starburst == b.starburst && a.starburstBlades == b.starburstBlades &&
           a.lightShafts == b.lightShafts && a.depthOfField == b.depthOfField && a.focusDistance == b.focusDistance &&
           a.focusTarget == b.focusTarget && a.motionBlur == b.motionBlur &&
           a.physicalSky == b.physicalSky && a.timeOfDay == b.timeOfDay && a.latitude == b.latitude && a.haze == b.haze &&
           a.aerialPerspective == b.aerialPerspective && a.cloudCover == b.cloudCover && a.ssgi == b.ssgi && a.ssgiIntensity == b.ssgiIntensity &&
           a.wetness == b.wetness && a.puddles == b.puddles && a.rain == b.rain && a.contactShadows == b.contactShadows;
}

bool sameLayerSettings(const LayerSettings& a, const LayerSettings& b) {
    for (int i = 0; i < LayerSettings::kCount; ++i)
        if (a.names[i] != b.names[i] || a.collides[i] != b.collides[i]) return false;
    return true;
}

bool sameNavigationSettings(const NavigationSettings& a, const NavigationSettings& b) {
    return a.cellSize == b.cellSize && a.agentRadius == b.agentRadius && a.agentHeight == b.agentHeight &&
           a.maxClimb == b.maxClimb && a.maxSlope == b.maxSlope;
}

bool sameAudioSettings(const AudioSettings& a, const AudioSettings& b) {
    return a.music == b.music && a.musicVolume == b.musicVolume && a.musicFade == b.musicFade &&
           a.reverbFromWorld == b.reverbFromWorld && a.reverbAmount == b.reverbAmount;
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
    if (!sameNavigationSettings(before.navigation, after.navigation)) d.navigation = before.navigation;
    if (!sameAudioSettings(before.audio, after.audio)) d.audio = before.audio;
    if (!sameLayerSettings(before.layers, after.layers)) d.layers = before.layers;
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
    if (d.navigation) scene.navigation = *d.navigation;
    if (d.audio) scene.audio = *d.audio;
    if (d.layers) scene.layers = *d.layers;
}
