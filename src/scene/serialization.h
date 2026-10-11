// Scene files: JSON (current), plus the line-based text format of earlier versions (load only).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/meshes.h"
#include "scene/scene.h"

namespace json { class Value; }

// Imported meshes and textures are stored as their source files and re-imported into the libraries on load.
bool saveScene(const std::string& path, const Scene& scene, const MeshLibrary& meshes, const TextureLibrary& textures,
               std::string& error);
bool loadScene(const std::string& path, Scene& scene, MeshLibrary& meshes, TextureLibrary& textures, std::string& error);

// The same as JSON documents (what a scene file holds); save games embed one
json::Value sceneToJson(const Scene& scene, const MeshLibrary& meshes, const TextureLibrary& textures);
bool sceneFromJson(const json::Value& doc, Scene& scene, MeshLibrary& meshes, TextureLibrary& textures, std::string& error);

// Prefab files (.prefab): one prefab plus the imported meshes and textures it uses
bool savePrefab(const std::string& path, const Prefab& prefab, const MeshLibrary& meshes, const TextureLibrary& textures,
                std::string& error);
bool loadPrefab(const std::string& path, Prefab& prefab, MeshLibrary& meshes, TextureLibrary& textures,
                std::string& error);

// Names used in a file -> library ids (meshes and textures are referenced by name)
struct AssetIds {
    std::map<std::string, int> meshes, textures;
};
AssetIds builtinAssetIds(const MeshLibrary& meshes, const TextureLibrary& textures);

// One behaviour <-> JSON ({"type": ..., fields...}). An unregistered type loads as a MissingBehaviour, which saves
// back exactly what it was loaded from.
json::Value behaviourToJson(const Behaviour& b);
std::unique_ptr<Behaviour> behaviourFromJson(const json::Value& v);

// A behaviour's runtime state() <-> JSON object (save games)
json::Value behaviourStateToJson(Behaviour& b);
void behaviourStateFromJson(Behaviour& b, const json::Value& v);

// Game module reloading: behaviours of the given types become MissingBehaviours (keeping their fields), before the
// code they live in is unloaded; afterwards, MissingBehaviours whose type is registered again become the real thing.
// Both cover the scene's entities and its prefabs. They return how many behaviours they changed.
int detachBehaviours(Scene& scene, const std::vector<std::string>& types);
int reattachBehaviours(Scene& scene);
int countMissingBehaviours(const Scene& scene);

// One entity <-> JSON
json::Value entityToJson(const Entity& e, const MeshLibrary& meshes, const TextureLibrary& textures);
bool entityFromJson(const json::Value& v, const AssetIds& ids, Entity& out, std::string& error);
