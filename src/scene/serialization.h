// Scene files: JSON (current), plus the line-based text format of earlier versions (load only).
#pragma once

#include <map>
#include <string>

#include "assets/meshes.h"
#include "scene/scene.h"

namespace json { class Value; }

// Imported meshes and textures are stored as their source files and re-imported into the libraries on load.
bool saveScene(const std::string& path, const Scene& scene, const MeshLibrary& meshes, const TextureLibrary& textures,
               std::string& error);
bool loadScene(const std::string& path, Scene& scene, MeshLibrary& meshes, TextureLibrary& textures, std::string& error);

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

// One entity <-> JSON
json::Value entityToJson(const Entity& e, const MeshLibrary& meshes, const TextureLibrary& textures);
bool entityFromJson(const json::Value& v, const AssetIds& ids, Entity& out, std::string& error);
