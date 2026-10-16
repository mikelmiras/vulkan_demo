// Flattens a scene into the renderer's draw list.
#pragma once

#include <vector>

#include "render/renderer.h"
#include "scene/scene.h"

// Every visible entity with a mesh renderer; `worlds` from Scene::worldMatrices(). `outlined` gets the selection outline.
// With `joints`, skinned meshes (a Skin on a skinned mesh) get their joint matrices appended there (else they're
// drawn in their bind pose).
std::vector<DrawItem> collectDrawItems(const Scene& scene, const std::vector<Mat4>& worlds,
                                       EntityId outlined = kNoEntity, std::vector<Mat4>* joints = nullptr,
                                       const MeshLibrary* meshes = nullptr,
                                       const std::vector<EntityId>* alsoOutlined = nullptr);
// Every visible entity with a light (directional lights shine along the entity's -Z)
std::vector<LightInfo> collectLights(const Scene& scene, const std::vector<Mat4>& worlds);
// Every visible terrain's foliage (scattered by terrain::syncMeshes): the sets point into the terrains' plants
std::vector<PlantSet> collectPlants(const Scene& scene, const std::vector<Mat4>& worlds, const MeshLibrary& meshes);
// Every visible reflection probe. `bakeCount` goes into their keys: a new value bakes them all again (the editor's Bake)
std::vector<ProbeInfo> collectProbes(const Scene& scene, const std::vector<Mat4>& worlds, uint32_t bakeCount = 0);
// Every visible particle emitter simulated on the GPU (ParticleEmitter::gpu)
std::vector<GpuEmitterInfo> collectGpuEmitters(const Scene& scene, const std::vector<Mat4>& worlds);
// Whether a draw item's material has layers (a coat, sheen, anisotropy, translucency, subsurface): drawn with the
// lit pass's layered variant, the rest with the lean one (scene.frag's kLayers)
bool hasMaterialLayers(const DrawItem& item);
// Whether a shadow caster lets the sun's light through, tinted, rather than blocking it: see-through ones under 75%
// opaque (glass, a stained window) and thin translucent ones (translucency 0.5 and over: paper, a curtain); drawn
// into the sun's tint layers instead of its depth (spot and point lights' shadows treat them as before)
bool tintsShadow(const DrawItem& item);
// The distance depth of field keeps sharp (RenderSettings): its focus target's along the view, if it has one (at
// least 0.1 m), else its focus distance
float focusDistance(const Scene& scene, const std::vector<Mat4>& worlds, Vec3 camPos, Vec3 camForward);
// Every visible decal (its box: the entity's transform on a unit cube)
std::vector<DecalInfo> collectDecals(const Scene& scene, const std::vector<Mat4>& worlds);
