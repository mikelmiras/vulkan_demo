// Procedural and imported (OBJ) meshes, kept on the CPU too so the editor can ray-pick against real triangles.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "assets/animation.h"
#include "core/vecmath.h"

struct Vertex {
    Vec3 pos, normal;
    float u = 0.f, v = 0.f;   // texture coordinates (v down, Vulkan style)
};

// How a vertex follows a skeleton: up to four joints (indices into the skin's joint list) and their weights.
// All zero for meshes that aren't skinned.
struct SkinVertex {
    uint16_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {0, 0, 0, 0};
};

// A simpler version of a mesh, drawn when it's small on screen (assets/mesh_lod.h)
struct MeshLod {
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
};

struct Mesh {
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
    std::vector<MeshLod> lods;   // levels 1, 2...: each about a quarter of the one before (none: always this)
    Vec3 boundsMin, boundsMax;   // local-space bounding box
    std::string name;
    std::string source;          // file it was imported from ("model.obj", "model.glb#2.0"); empty for built-ins
    bool skinned = false;        // has joint weights (deformed by a Skin component)
};

// Built-in meshes: their ids in the library. Imported meshes get ids from MeshKind::Count up.
enum class MeshKind : int { Plane, Cube, Sphere, Pyramid, Torus, Count };

// All meshes share one vertex buffer and one index buffer. Meshes are only ever appended, so ids stay valid.
struct MeshLibrary {
    std::vector<Vertex> vertices;
    std::vector<SkinVertex> skin;   // aligned with `vertices`
    std::vector<uint32_t> indices;
    std::vector<Mesh> meshes;
    std::vector<AnimationClip> animations;   // imported with the models (never removed: indices stay valid)
    uint32_t version = 0;   // bumped whenever meshes are added (the renderer re-uploads)
    // Vertices rewritten in place (terrains being sculpted): first vertex -> (count, serial). The renderer copies
    // those with a serial newer than the last it saw (rewriteSerial is the latest).
    std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>> rewritten;
    uint64_t rewriteSerial = 0;

    int count() const { return (int)meshes.size(); }
    const Mesh& get(int id) const { return meshes[(size_t)id]; }
    const char* name(int id) const { return meshes[(size_t)id].name.c_str(); }
    int find(const std::string& name) const;   // -1 if missing
    const AnimationClip* findAnimation(const std::string& name) const;   // nullptr if missing
};

MeshLibrary buildMeshLibrary();

// Loads a Wavefront OBJ (positions, optional normals; polygons are triangulated) and appends it to the library,
// recentred and scaled to fit a unit cube like the built-in meshes. Re-importing the same file returns the
// existing id. Returns the new mesh id, or -1 with `error` set.
int importObj(MeshLibrary& lib, const std::string& path, std::string& error);

// Appends a mesh as is (no recentring). Used by importers; returns its id.
int addMesh(MeshLibrary& lib, const std::string& name, const std::string& source, const std::vector<Vertex>& vertices,
            const std::vector<uint32_t>& indices);
// The same with joint weights (one per vertex): a mesh for a Skin component
int addSkinnedMesh(MeshLibrary& lib, const std::string& name, const std::string& source,
                   const std::vector<Vertex>& vertices, const std::vector<SkinVertex>& skin,
                   const std::vector<uint32_t>& indices);
// Re-imports a mesh from its source: an OBJ path or a glTF primitive ("model.glb#mesh.primitive"). Already
// loaded sources return their existing id; -1 with `error` on failure.
int importMesh(MeshLibrary& lib, const std::string& source, std::string& error);
// Reads an OBJ mesh again (after its import settings changed), keeping its id; -1 with `error` on failure
int reloadMesh(MeshLibrary& lib, int id, std::string& error);

// Ray vs. mesh in the mesh's local space. Returns the ray parameter t of the closest hit, or -1, and the hit
// triangle's unit face normal if `normal` is given (it may face either way: meshes are double-sided).
float intersectMesh(const MeshLibrary& lib, int mesh, Vec3 origin, Vec3 dir, Vec3* normal = nullptr);
