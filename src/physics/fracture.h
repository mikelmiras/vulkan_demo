// Pre-fractured objects (the Fracture component, scene/scene.h): a mesh cut into pieces ahead of time, by planes
// through the biggest piece so far (random directions from the seed: the same pieces every time), each cut face
// closed with a flat cap. Convex meshes (boxes, rocks, crates) cut cleanly; a concave mesh's caps may cross its gaps.
#pragma once

#include <vector>

#include "assets/meshes.h"
#include "scene/scene.h"

namespace fracture {

struct Piece {
    std::vector<Vertex> vertices;   // (triangles: every three)
    Vec3 centre;                    // the average of its corners (where it flies away from the whole's centre)
};

// Splits the triangles in two along a plane (n . p = d): the side n points to, and the other; the cut closed on both
std::pair<Piece, Piece> split(const Piece& piece, Vec3 n, float d);
// A mesh's triangles cut into `count` pieces
std::vector<Piece> cut(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, int count, uint32_t seed);

// Makes the pieces' meshes of the scene's Fracture components (once per mesh, count and seed)
void syncMeshes(Scene& scene, MeshLibrary& meshes);

}  // namespace fracture
