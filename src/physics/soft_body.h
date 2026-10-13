// Soft bodies (the SoftBody component, scene/scene.h): what Jolt simulates (particles, triangles, which particles are
// pinned) and what's drawn (a mesh whose vertices follow the particles), built from the component and its mesh.
#pragma once

#include <vector>

#include "assets/meshes.h"
#include "scene/scene.h"

namespace softbody {

struct Build {
    std::vector<Vec3> particles;       // rest positions (entity space)
    std::vector<float> invMass;        // 0: pinned
    std::vector<uint32_t> faces;       // particle triangles
    std::vector<Vertex> vertices;      // the drawn mesh at rest
    std::vector<uint32_t> indices;
    std::vector<uint32_t> particleOf;  // drawn vertex -> its particle
    std::vector<float> side;           // drawn vertex: 1, or -1 for a cloth's back face (its normal turned round)
    std::vector<int> faceOf;           // drawn vertex -> the face it's a corner of (one that tears: none shared)
};

// Cloth: a grid; Mesh: the mesh's vertices welded by position into particles (needs `mesh`). False if there's
// nothing to build (no mesh, too few particles).
bool build(const SoftBody& s, const MeshLibrary& meshes, int mesh, Build& out);
// The triangles stretched past tearStretch (not already torn), given the particles' rest and current positions
std::vector<uint32_t> tearing(const std::vector<Vec3>& rest, const std::vector<Vec3>& now, const std::vector<uint32_t>& faces,
                              const std::vector<uint32_t>& torn, float tearStretch);
// Smooth normals of the particles from their triangles
std::vector<Vec3> particleNormals(const std::vector<Vec3>& particles, const std::vector<uint32_t>& faces);

// Makes (or rewrites in place, as the particles move) the drawn meshes of the scene's soft bodies
void syncMeshes(Scene& scene, MeshLibrary& meshes);

}  // namespace softbody
