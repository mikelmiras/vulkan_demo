#include "render/scene_draw.h"

#include <unordered_map>

std::vector<DrawItem> collectDrawItems(const Scene& scene, const std::vector<Mat4>& worlds, EntityId outlined,
                                       std::vector<Mat4>* joints, const MeshLibrary* meshes) {
    std::vector<DrawItem> items;
    if (joints) joints->clear();
    std::unordered_map<EntityId, std::unordered_map<std::string, EntityId>> skeletons;   // model root -> names
    for (size_t i = 0; i < scene.entities.size() && i < worlds.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.meshRenderer || !scene.visibleInHierarchy(e.id)) continue;
        DrawItem d;
        d.model = worlds[i];
        d.mesh = e.meshRenderer->mesh;
        d.color = e.meshRenderer->color;
        d.texture = e.meshRenderer->texture;
        d.tiling = e.meshRenderer->tiling;
        d.checker = e.meshRenderer->checker;
        d.outlined = e.id == outlined && outlined != kNoEntity;
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
        lights.push_back(l);
    }
    return lights;
}
