// Keeping prefab instances in sync with their prefab.
//
// An instance is the entities instantiate() made from a prefab: each carries a PrefabLink to the prefab entity it
// was copied from. Overrides are implicit: a property of an instance that still equals the prefab's value follows
// the prefab when it changes; a property the instance changed (position, colour, a behaviour field, a removed
// component...) is an override and is kept. Properties are compared as their JSON form, field by field.
#pragma once

#include <string>
#include <vector>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "scene/scene.h"

// The root of the prefab instance `id` belongs to (the entity linked to the prefab's root), or kNoEntity
EntityId instanceRoot(const Scene& scene, EntityId id);

// Makes the prefab look like the entity tree under `root` (an instance of it, or any entity) and updates the
// other instances. Entities linked to the prefab keep their identity, so the instances' overrides survive; the
// tree under `root` becomes a linked instance. Returns how many other instances were updated, or -1 if there's
// no such prefab.
int applyToPrefab(Scene& scene, EntityId root, uint32_t prefabId, const MeshLibrary& meshes,
                  const TextureLibrary& textures);

// After a prefab changed from `before` to `after` (same id): updates every instance in the scene except `skipRoot`.
// Entities added to / removed from the prefab are added to / removed from the instances. Returns how many
// instances were updated.
int syncPrefabInstances(Scene& scene, const Prefab& before, const Prefab& after, const MeshLibrary& meshes,
                        const TextureLibrary& textures, EntityId skipRoot = kNoEntity);

// The same, then into the prefabs that contain instances of it (nested prefabs, variants) and on to their
// instances, recursively. applyToPrefab does this.
int propagatePrefabChange(Scene& scene, const Prefab& before, const Prefab& after, const MeshLibrary& meshes,
                          const TextureLibrary& textures, EntityId skipRoot = kNoEntity, int depth = 0);

// Drops the instance's overrides (except where its root is placed and its name) and restores entities the instance
// deleted. Entities added to the instance stay.
void revertInstance(Scene& scene, EntityId root, const MeshLibrary& meshes, const TextureLibrary& textures);

// Turns the instance into ordinary entities
void unlinkInstance(Scene& scene, EntityId root);

// Properties of `id` that differ from its prefab entity, e.g. "transform.position", "meshRenderer.color"
std::vector<std::string> instanceOverrides(const Scene& scene, EntityId id, const MeshLibrary& meshes,
                                           const TextureLibrary& textures);
