#include "render/scene_draw.h"

std::vector<DrawItem> collectDrawItems(const Scene& scene, const std::vector<Mat4>& worlds, EntityId outlined) {
    std::vector<DrawItem> items;
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
