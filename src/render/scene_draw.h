// Flattens a scene into the renderer's draw list.
#pragma once

#include <vector>

#include "render/renderer.h"
#include "scene/scene.h"

// Every visible entity with a mesh renderer; `worlds` from Scene::worldMatrices(). `outlined` gets the selection outline.
std::vector<DrawItem> collectDrawItems(const Scene& scene, const std::vector<Mat4>& worlds,
                                       EntityId outlined = kNoEntity);
// Every visible entity with a light (directional lights shine along the entity's -Z)
std::vector<LightInfo> collectLights(const Scene& scene, const std::vector<Mat4>& worlds);
