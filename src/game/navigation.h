// Navigation: where agents can walk, and paths there.
//
// A NavGrid is baked from the scene's static level geometry: solid colliders on entities without a RigidBody or
// behaviours (moving things and characters aren't floors or walls). Every grid column is probed top to bottom for
// surfaces that are flat enough with enough headroom, so floors can stack (bridges, upper storeys). Cells link to
// their neighbours when the step between them is low enough, and cells closer than the agent's radius to an edge
// or a wall are dropped, so paths keep clear of them. findPath() runs A* over the cells and straightens the result
// (it only turns where it must).
#pragma once

#include <cstdint>
#include <vector>

#include "assets/meshes.h"
#include "scene/scene.h"

class NavGrid {
public:
    // Bakes the scene's walkable cells; false if there's nothing to walk on
    bool bake(const Scene& scene, const MeshLibrary& meshes, const NavigationSettings& settings);
    void clear();
    bool empty() const { return cells_.empty(); }

    // A walking path from `from` to `to` (positions at or a little above the ground, like a character's centre):
    // corner points on the ground, starting with where `from` stands and ending with the nearest spot to `to`.
    // False if `from` isn't near walkable ground or `to` can't be reached from there.
    bool findPath(Vec3 from, Vec3 to, std::vector<Vec3>& path) const;
    // The walkable spot nearest to `p` (within maxDistance horizontally); false if none
    bool nearest(Vec3 p, Vec3& onGround, float maxDistance = 2.f) const;
    // Whether a straight walk from a to b (both on the grid) stays on walkable ground
    bool walkable(Vec3 a, Vec3 b) const;

    size_t cellCount() const { return cells_.size(); }
    float bakeMs() const { return bakeMs_; }
    const NavigationSettings& settings() const { return settings_; }
    // For drawing: each cell's centre on the ground
    std::vector<Vec3> cellCenters() const;
    // ... and each link between two cells (as pairs of points, once each)
    std::vector<Vec3> links() const;

private:
    struct Cell {
        float y;
        int32_t next[8];   // linked neighbour cells (or -1), directions as kDirs in navigation.cpp
    };
    int columnOf(float x, float z) const;   // -1 outside the grid
    int cellAt(Vec3 p, float searchRadius) const;
    Vec3 centre(int cell) const;
    int cellColumn(int cell) const;

    NavigationSettings settings_;
    Vec3 origin_;        // the grid's corner (x, z), min height
    int nx_ = 0, nz_ = 0;
    float cell_ = 0.4f;
    std::vector<int32_t> columnStart_;   // nx * nz + 1: the cells of column c are [columnStart[c], columnStart[c+1])
    std::vector<Cell> cells_;            // by column, lowest first
    std::vector<int32_t> cellColumn_;
    float bakeMs_ = 0.f;
    struct Search {
        float g;
        int32_t parent;
        uint32_t generation;
        bool closed;
    };
    mutable std::vector<Search> search_;   // findPath's scratch (so a NavGrid isn't safe to search from two threads)
    mutable uint32_t generation_ = 0;
};
