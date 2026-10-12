// Levels of detail: simpler versions of a mesh for when it's small on screen, made by vertex clustering (the
// mesh's box is cut into a grid; the vertices in each cell, with a similar normal and texture position, merge
// into one; triangles left with fewer than three corners go). Seams in texture coordinates and sharp edges keep
// their sides apart. Coarse, but quick and good enough for what's a few pixels tall.
#pragma once

#include <vector>

#include "assets/meshes.h"

constexpr uint32_t kLodMinTriangles = 1500;   // smaller meshes get no levels: they're cheap already
constexpr int kMaxLods = 2;                   // levels beyond the mesh itself

// One clustered version of the vertices / triangles: `cells` grid cells along the box's longest side
void clusterSimplify(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, int cells,
                     std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices);

// Gives a mesh its levels (appended to the library's vertices and indices): about 1/4 and 1/16 of its triangles.
// Skinned meshes and ones under kLodMinTriangles are left alone; levels that barely simplify are dropped.
void generateLods(MeshLibrary& lib, int mesh);

// Which level to draw: from how much of the view's height its bounding sphere covers (radius / (distance *
// tan(fovY / 2))). Level 1 below kLod1Size, 2 below kLod2Size; never more than the mesh has.
constexpr float kLod1Size = 0.12f, kLod2Size = 0.04f;
int chooseLod(float radius, float distance, float tanHalfFov, int levels);
