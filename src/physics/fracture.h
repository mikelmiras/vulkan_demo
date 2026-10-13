// Pre-fractured objects (the Fracture component, scene/scene.h): a mesh cut into pieces ahead of time, by planes
// through the biggest piece so far (random directions from the seed: the same pieces every time), each cut face
// closed with caps (its outline's loops, holes included, triangulated: concave meshes cut cleanly too). A concave
// piece also gets convex parts (convexParts), whose hulls together are its moving collider.
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
// Whether every corner is behind (or on) every triangle's plane (within `tolerance` of its size)
bool isConvex(const Piece& piece, float tolerance = 0.01f);
// An approximate convex decomposition: a concave piece split through its middle across its longest side, again
// and again, until the parts are convex or there are `maxParts` (a single part: it was convex)
std::vector<Piece> convexParts(const Piece& piece, int maxParts = 8);

// Makes the pieces' meshes of the scene's Fracture components (once per mesh, count and seed)
void syncMeshes(Scene& scene, MeshLibrary& meshes);

}  // namespace fracture
