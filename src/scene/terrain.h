// Terrain (the component is in scene/scene.h): making, sampling, sculpting and painting height grids, their meshes,
// and their compact encoding in scene files. Positions here are in the terrain's local space (the entity's):
// x and z from -size / 2 to size / 2, y the height.
#pragma once

#include <cstdint>
#include <string>

#include "scene/scene.h"

namespace terrain {

constexpr int kResolutions[] = {64, 128, 256};   // samples per side (Jolt height fields want powers of two)

void init(Terrain& t, int resolution, float size);   // flat at 0, all of layer 0
void resize(Terrain& t, int resolution);              // resampled (heights and paint)
void touch(Terrain& t);                               // after changing it: a new revision (redrawn, new collision)

float cellSize(const Terrain& t);
Vec3 pointPosition(const Terrain& t, int x, int z);
// The surface's height at (x, z), as drawn and as it collides (the grid's triangles); clamped at the edges
float heightAt(const Terrain& t, float x, float z);
bool holeAt(const Terrain& t, float x, float z);   // the triangle there is cut away (it uses a hole point)
Vec3 normalAt(const Terrain& t, float x, float z);
// Where a ray (local space) first meets the surface: its distance along `dir` (normalised); false if it misses
bool raycast(const Terrain& t, Vec3 origin, Vec3 dir, float maxDistance, float& hit);

// The editor's brushes. Strength is per second (heights: metres per second at full strength); radius in metres.
// Holes cut the ground away (Fill puts it back); Foliage paints how much of a foliage layer grows (erase: less).
enum class Brush { Raise, Lower, Smooth, Flatten, Paint, Holes, Fill, Foliage };
struct BrushSettings {
    Brush mode = Brush::Raise;
    float radius = 4.f, strength = 0.5f;
    int layer = 1;              // Paint / Foliage: which layer
    float flattenHeight = 0.f;  // Flatten: to this height
    bool erase = false;         // Foliage: thin it out instead
};
bool applyBrush(Terrain& t, const BrushSettings& b, Vec3 centre, float dt);   // true if it changed anything
// Fractal hills added on top: up to `amplitude` metres, features about `scale` metres across
void addNoise(Terrain& t, float amplitude, float scale, uint32_t seed);

uint64_t contentHash(const Terrain& t);   // of what it looks like (not its revision): same terrain, same hash

// Drawing: the terrain in chunks of kChunkCells x kChunkCells cells, a mesh each over one shared grid of vertices
// (plus skirts hanging down from chunk edges, so chunks at different detail leave no gaps), with two coarser
// levels of detail that skip points (every 2nd, every 4th). Hole points are flagged in the vertices (the shader
// drops their triangles; chunks with holes keep full detail).
constexpr int kChunkCells = 32;
// Builds (or rewrites in place) the meshes of the scene's terrains that changed since, makes the meshes their
// foliage names that are built in ("Grass Blade") and scatters their foliage again. Call before drawing it.
void syncMeshes(Scene& scene, MeshLibrary& meshes);
// The foliage of one terrain, as syncMeshes leaves it in t.plants / t.plantRanges: chunk by chunk, layer by layer
void scatterFoliage(Terrain& t);
int grassBladeMesh(MeshLibrary& meshes);   // the built-in clump of blades (made the first time)

// A heightmap image (8 or 16-bit grey; colour is averaged) as the heights: black at 0, white at `height` metres,
// resampled to the terrain's grid. False with `error` if it can't be read.
bool importHeightmap(Terrain& t, const std::string& path, float height, std::string& error);

// Scene files: heights as 16-bit steps between their lowest and highest, and the paint, compressed and in base64
std::string encodeHeights(const Terrain& t, float& lowest, float& highest);
bool decodeHeights(Terrain& t, const std::string& text, float lowest, float highest);
std::string encodeWeights(const Terrain& t);
bool decodeWeights(Terrain& t, const std::string& text);
std::string encodeBytes(const std::vector<uint8_t>& bytes);   // (holes, foliage density)
bool decodeBytes(const std::string& text, size_t expected, std::vector<uint8_t>& out);

}  // namespace terrain
