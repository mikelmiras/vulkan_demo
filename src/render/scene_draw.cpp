#include "render/scene_draw.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

std::vector<DrawItem> collectDrawItems(const Scene& scene, const std::vector<Mat4>& worlds, EntityId outlined,
                                       std::vector<Mat4>* joints, const MeshLibrary* meshes,
                                       const std::vector<EntityId>* alsoOutlined) {
    std::vector<DrawItem> items;
    if (joints) joints->clear();
    std::unordered_map<EntityId, std::unordered_map<std::string, EntityId>> skeletons;   // model root -> names
    for (size_t i = 0; i < scene.entities.size() && i < worlds.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (e.terrain && !e.terrain->meshes.empty() && scene.visibleInHierarchy(e.id)) {   // (from terrain::syncMeshes)
            DrawItem d;
            d.model = worlds[i];
            d.mesh = e.terrain->meshes[0];
            d.terrain = true;
            d.roughness = 0.9f;
            d.outlined = (e.id == outlined && outlined != kNoEntity) ||
                         (alsoOutlined && std::find(alsoOutlined->begin(), alsoOutlined->end(), e.id) != alsoOutlined->end());
            if (joints) {
                Mat4 layers;
                for (int k = 0; k < Terrain::kLayers; ++k) {
                    const Terrain::Layer& l = e.terrain->layers[k];
                    d.layerTextures[k] = l.texture;
                    Vec3 tint = srgbToLinear(l.color);
                    layers.m[k * 4 + 0] = tint.x;
                    layers.m[k * 4 + 1] = tint.y;
                    layers.m[k * 4 + 2] = tint.z;
                    layers.m[k * 4 + 3] = l.tiling;
                }
                d.terrainLayers = (int)joints->size();
                joints->push_back(layers);
            }
            for (int chunk : e.terrain->meshes) {   // a draw per chunk: culled and given a level of detail on its own
                if (meshes && (chunk < 0 || chunk >= meshes->count())) continue;
                d.mesh = chunk;
                items.push_back(d);
            }
        }
        if (e.water && e.water->mesh >= 0 && (!meshes || e.water->mesh < meshes->count()) && scene.visibleInHierarchy(e.id)) {
            // Water (scene/water.h): its grid, the size of its surface, at the entity's place and turn (not its scale)
            const Water& w = *e.water;
            const Mat4& m = worlds[i];
            float yaw = std::atan2(-m.m[2], m.m[0]);
            Mat4 size;
            size.m[0] = w.width;
            size.m[10] = w.length;
            DrawItem d;
            d.model = translate({m.m[12], m.m[13], m.m[14]}) * rotateAxis({0.f, 1.f, 0.f}, yaw) * size;
            d.mesh = w.mesh;
            d.color = w.color;
            d.roughness = 0.04f;
            d.alphaMode = 2;
            d.opacity = 1.f - std::clamp(w.clarity, 0.f, 1.f) * 0.85f;
            d.water = true;
            d.waveHeight = std::max(w.waveHeight, 0.f);
            d.waveLength = std::max(w.waveLength, 0.05f);
            d.waveDirection = w.waveDirection * DEG2RAD - yaw;
            d.choppiness = std::clamp(w.choppiness, 0.f, 1.f);
            d.current = w.current;
            d.outlined = (e.id == outlined && outlined != kNoEntity) ||
                         (alsoOutlined && std::find(alsoOutlined->begin(), alsoOutlined->end(), e.id) != alsoOutlined->end());
            items.push_back(d);
        }
        if (!e.meshRenderer || !scene.visibleInHierarchy(e.id)) continue;
        DrawItem d;
        d.model = worlds[i];
        d.mesh = e.meshRenderer->mesh;
        if (e.softBody && e.softBody->mesh >= 0 && (!meshes || e.softBody->mesh < meshes->count()))
            d.mesh = e.softBody->mesh;   // (its own deforming copy: physics/soft_body.h)
        d.color = e.meshRenderer->color;
        d.texture = e.meshRenderer->texture;
        d.tiling = e.meshRenderer->tiling;
        d.checker = e.meshRenderer->checker;
        d.outlined = (e.id == outlined && outlined != kNoEntity) ||
                     (alsoOutlined && std::find(alsoOutlined->begin(), alsoOutlined->end(), e.id) != alsoOutlined->end());
        const MeshRenderer& mr = *e.meshRenderer;
        d.metallic = mr.metallic;
        d.roughness = mr.roughness;
        d.metalRoughTexture = mr.metalRoughTexture;
        d.normalTexture = mr.normalTexture;
        d.normalScale = mr.normalScale;
        d.emissiveTexture = mr.emissiveTexture;
        d.emissive = srgbToLinear(mr.emissive) * mr.emissiveStrength;
        d.alphaMode = (int)mr.alphaMode;
        d.opacity = mr.opacity;
        d.alphaCutoff = mr.alphaCutoff;
        if (joints && e.skin && meshes && d.mesh >= 0 && d.mesh < meshes->count() && meshes->get(d.mesh).skinned) {
            // Joint matrix = (the mesh's world)^-1 * the joint's world * inverse bind: the shader applies the model
            // matrix on top, so a skinned vertex lands where its joints put it
            EntityId root = scene.modelRoot(e.id);
            auto it = skeletons.find(root);
            if (it == skeletons.end()) it = skeletons.emplace(root, scene.namesUnder(root)).first;
            Mat4 toMesh = inverseAffine(d.model);
            d.jointOffset = (int)joints->size();
            for (size_t j = 0; j < e.skin->joints.size(); ++j) {
                auto jt = it->second.find(e.skin->joints[j]);
                int ji = jt != it->second.end() ? scene.indexOf(jt->second) : -1;
                Mat4 ibm = j < e.skin->inverseBind.size() ? e.skin->inverseBind[j] : Mat4{};
                joints->push_back(ji >= 0 && ji < (int)worlds.size() ? toMesh * worlds[(size_t)ji] * ibm : Mat4{});
            }
            if (e.skin->joints.empty()) d.jointOffset = -1;
        }
        items.push_back(d);
    }
    return items;
}

std::vector<PlantSet> collectPlants(const Scene& scene, const std::vector<Mat4>& worlds, const MeshLibrary& meshes) {
    std::vector<PlantSet> sets;
    for (size_t i = 0; i < scene.entities.size() && i < worlds.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.terrain || e.terrain->plants.empty() || !scene.visibleInHierarchy(e.id)) continue;
        const Terrain& t = *e.terrain;
        PlantSet s;
        s.key = t.plantRevision ^ ((uint64_t)e.id << 40);
        s.plants = &t.plants[0].pos[0];
        s.count = t.plants.size();
        s.model = worlds[i];
        for (const Terrain::PlantRange& r : t.plantRanges) {
            if (r.layer < 0 || r.layer >= (int)t.foliage.size()) continue;
            const Terrain::Foliage& f = t.foliage[(size_t)r.layer];
            PlantSet::Range out;
            out.mesh = meshes.find(f.mesh);
            if (out.mesh < 0) continue;
            out.first = r.first;
            out.count = r.count;
            out.lo = r.lo;
            out.hi = r.hi;
            out.color = f.color;
            out.wind = f.wind;
            out.fade = f.fadeDistance;
            out.maxScale = std::max(f.maxScale, f.minScale);
            s.ranges.push_back(out);
        }
        sets.push_back(std::move(s));
    }
    return sets;
}

std::vector<LightInfo> collectLights(const Scene& scene, const std::vector<Mat4>& worlds) {
    std::vector<LightInfo> lights;
    for (size_t i = 0; i < scene.entities.size() && i < worlds.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.light || !scene.visibleInHierarchy(e.id)) continue;
        const Mat4& m = worlds[i];
        LightInfo l;
        l.directional = e.light->type == Light::Type::Directional;
        l.spot = e.light->type == Light::Type::Spot;
        l.spotAngle = e.light->spotAngle;
        l.spotSoftness = e.light->spotSoftness;
        l.position = {m.m[12], m.m[13], m.m[14]};
        l.direction = normalize(transformDir(m, {0, 0, -1}));
        l.color = e.light->color;
        l.intensity = e.light->intensity;
        l.range = e.light->range;
        l.castShadows = e.light->castShadows;
        if (l.spot) l.cookie = e.light->cookie;
        lights.push_back(l);
    }
    return lights;
}

std::vector<ProbeInfo> collectProbes(const Scene& scene, const std::vector<Mat4>& worlds, uint32_t bakeCount) {
    std::vector<ProbeInfo> probes;
    for (size_t i = 0; i < scene.entities.size() && i < worlds.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.reflectionProbe || !scene.visibleInHierarchy(e.id)) continue;
        const ReflectionProbe& r = *e.reflectionProbe;
        ProbeInfo p;
        p.position = {worlds[i].m[12], worlds[i].m[13], worlds[i].m[14]};
        Vec3 half{std::fabs(r.size.x) * 0.5f, std::fabs(r.size.y) * 0.5f, std::fabs(r.size.z) * 0.5f};
        p.boxMin = p.position - half;
        p.boxMax = p.position + half;
        p.resolution = r.resolution;
        p.boxProjection = r.boxProjection;
        uint64_t h = 1469598103934665603ull;   // (what it shows: a new key, a new bake)
        auto add = [&](const void* d, size_t n) {
            for (size_t k = 0; k < n; ++k) h = (h ^ static_cast<const uint8_t*>(d)[k]) * 1099511628211ull;
        };
        add(&e.id, sizeof(e.id));
        add(&p.position, sizeof(p.position));
        add(&half, sizeof(half));
        add(&p.resolution, sizeof(p.resolution));
        add(&bakeCount, sizeof(bakeCount));
        p.key = h;
        probes.push_back(p);
    }
    return probes;
}
