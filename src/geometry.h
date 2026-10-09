// Procedural meshes, kept on the CPU too so the editor can ray-pick against real triangles.
#pragma once

#include <cstdint>
#include <vector>

#include "vecmath.h"

struct Vertex { Vec3 pos, normal; };

struct Mesh {
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexOffset = 0;
};

enum class MeshKind : int { Plane, Cube, Sphere, Pyramid, Torus, Count };

const char* meshKindName(MeshKind kind);

// All meshes share one vertex buffer and one index buffer.
struct MeshLibrary {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Mesh meshes[(int)MeshKind::Count];

    const Mesh& get(MeshKind kind) const { return meshes[(int)kind]; }
};

MeshLibrary buildMeshLibrary();

// Ray vs. mesh in the mesh's local space. Returns the ray parameter t of the closest hit, or -1.
float intersectMesh(const MeshLibrary& lib, MeshKind kind, Vec3 origin, Vec3 dir);
