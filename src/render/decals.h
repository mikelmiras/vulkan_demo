// Which decals the shaders get (render/renderer.h: DecalInfo, Renderer::kMaxDecals): the nearest to the camera,
// counting from the edge of their box, and the textures they use, each a slot in a small array (set 0, binding 25;
// normal maps: binding 28).
// Kept apart from the renderer so it can be tested without a GPU.
#pragma once

#include <vector>

#include "render/renderer.h"

struct DecalPick {
    std::vector<int> decals;     // indices into the decals given, nearest first
    std::vector<int> slots;      // per picked decal: its texture's slot (-1: none, or no slot left: its colour alone)
    std::vector<int> textures;   // per slot: the texture id
    std::vector<int> normalSlots;      // the same for their normal maps
    std::vector<int> normalTextures;
};
DecalPick pickDecals(const std::vector<DecalInfo>& decals, Vec3 camPos, size_t maxDecals, size_t maxTextures,
                     size_t maxNormalTextures = 0);
