// Scene files: JSON (current), plus the line-based text format of earlier versions (load only).
#pragma once

#include <map>
#include <string>

#include "assets/meshes.h"
#include "scene/scene.h"

namespace json { class Value; }

// Imported meshes are stored as their OBJ paths and re-imported into `meshes` on load.
bool saveScene(const std::string& path, const Scene& scene, const MeshLibrary& meshes, std::string& error);
bool loadScene(const std::string& path, Scene& scene, MeshLibrary& meshes, std::string& error);

// Prefab files (.prefab): one prefab plus the imported meshes it uses
bool savePrefab(const std::string& path, const Prefab& prefab, const MeshLibrary& meshes, std::string& error);
bool loadPrefab(const std::string& path, Prefab& prefab, MeshLibrary& meshes, std::string& error);

// One entity <-> JSON. Meshes are referenced by name; `meshIds` maps the names used in the file to library ids.
json::Value entityToJson(const Entity& e, const MeshLibrary& meshes);
bool entityFromJson(const json::Value& v, const std::map<std::string, int>& meshIds, Entity& out, std::string& error);
