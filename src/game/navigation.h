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
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

#include "assets/meshes.h"
#include "scene/scene.h"

// The level as baking needs it: the scene's static colliders and navigation links, with copies of the meshes they
// use, so a bake can run on another thread while the game goes on. `hash` identifies it (a saved bake is reused
// only for the same level).
struct NavLevel {
    Scene scene;
    MeshLibrary meshes;
    NavigationSettings settings;
    uint64_t hash = 0;
};
NavLevel prepareNavLevel(const Scene& scene, const MeshLibrary& meshes);

class NavGrid {
public:
    // Bakes the scene's walkable cells; false if there's nothing to walk on
    bool bake(const Scene& scene, const MeshLibrary& meshes, const NavigationSettings& settings);
    bool bake(const NavLevel& level);
    void clear();
    bool empty() const { return cells_.empty(); }

    // A walking path from `from` to `to` (positions at or a little above the ground, like a character's centre):
    // corner points on the ground, starting with where `from` stands and ending with the nearest spot to `to`.
    // False if `from` isn't near walkable ground or `to` can't be reached from there.
    // With `jumps`, jumps[i] says the way from path[i] to path[i + 1] is an off-mesh link (a hop, not a walk).
    bool findPath(Vec3 from, Vec3 to, std::vector<Vec3>& path, std::vector<uint8_t>* jumps = nullptr) const;
    // Obstacles (world boxes: things that move, like crates): the cells under them, widened by the agent's radius,
    // are out of bounds until the next call
    void setObstacles(const std::vector<std::pair<Vec3, Vec3>>& boxes);
    bool blocked(Vec3 p) const;   // on a cell an obstacle covers

    // A bake in a file, for a level with this hash: loading one that doesn't match fails
    bool save(const std::filesystem::path& path) const;
    bool load(const std::filesystem::path& path, uint64_t hash);
    uint64_t levelHash() const { return hash_; }
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
    std::vector<std::pair<Vec3, Vec3>> offMeshLinks() const;   // (start, end) of each

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
    struct OffLink {
        int32_t a, b;   // cells at its ends
        Vec3 pa, pb;
        float cost;
        bool bothWays;
    };
    std::vector<OffLink> offLinks_;
    std::unordered_map<int32_t, std::vector<int32_t>> linksAt_;   // cell -> the off-mesh links leaving it
    std::vector<uint8_t> blocked_;   // per cell: under an obstacle
    uint64_t hash_ = 0;
    float bakeMs_ = 0.f;
    struct Search {
        float g;
        int32_t parent;
        uint32_t generation;
        bool closed;
        int32_t via;   // the off-mesh link it was reached by, or -1
    };
    void straighten(const std::vector<Vec3>& points, std::vector<Vec3>& out) const;
    mutable std::vector<Search> search_;   // findPath's scratch (so a NavGrid isn't safe to search from two threads)
    mutable uint32_t generation_ = 0;
};
