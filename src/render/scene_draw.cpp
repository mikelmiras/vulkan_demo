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
        d.checker = e.meshRenderer->checker;
        d.outlined = e.id == outlined && outlined != kNoEntity;
        items.push_back(d);
    }
    return items;
}
