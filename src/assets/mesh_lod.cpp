#include "assets/mesh_lod.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

void clusterSimplify(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, int cells,
                     std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices) {
    outVertices.clear();
    outIndices.clear();
    if (vertices.empty()) return;
    Vec3 lo = vertices[0].pos, hi = lo;
    for (const Vertex& v : vertices) {
        lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
        hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
    }
    float side = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-6f});
    float cell = side / (float)std::max(cells, 1);
    // A vertex's cluster: its grid cell, which way its normal points (26 directions + the dominant axis' sign) and
    // a coarse texture cell (so a seam's two sides, far apart in texture space, stay two vertices)
    struct Sum {
        Vec3 pos, normal;
        float u = 0.f, v = 0.f;
        int n = 0;
    };
    std::unordered_map<uint64_t, uint32_t> clusterOf;
    std::vector<Sum> sums;
    std::vector<uint32_t> remap(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vertex& v = vertices[i];
        auto q = [&](float x, float o) { return (uint64_t)std::clamp((int)((x - o) / cell), 0, 2047); };
        auto dir = [](float x) { return (uint64_t)(x > 0.38f ? 2 : x < -0.38f ? 0 : 1); };   // (about 22 degrees off an axis)
        auto tex = [](float t) { return (uint64_t)((int)std::floor(t * 4.f) & 63); };
        uint64_t key = q(v.pos.x, lo.x) | q(v.pos.y, lo.y) << 11 | q(v.pos.z, lo.z) << 22 |
                       (dir(v.normal.x) + 3 * dir(v.normal.y) + 9 * dir(v.normal.z)) << 33 | tex(v.u) << 38 | tex(v.v) << 44;
        auto [it, added] = clusterOf.try_emplace(key, (uint32_t)sums.size());
        if (added) sums.emplace_back();
        Sum& s = sums[it->second];
        s.pos = s.pos + v.pos;
        s.normal = s.normal + v.normal;
        s.u += v.u;
        s.v += v.v;
        ++s.n;
        remap[i] = it->second;
    }
    outVertices.reserve(sums.size());
    for (const Sum& s : sums) {
        float inv = 1.f / (float)s.n;
        Vertex v;
        v.pos = s.pos * inv;
        v.normal = length(s.normal) > 1e-6f ? normalize(s.normal) : Vec3{0, 1, 0};
        v.u = s.u * inv;
        v.v = s.v * inv;
        outVertices.push_back(v);
    }
    // Triangles that still have three corners (once each)
    std::unordered_map<uint64_t, bool> seen;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        uint32_t a = remap[indices[t]], b = remap[indices[t + 1]], c = remap[indices[t + 2]];
        if (a == b || b == c || a == c) continue;
        uint32_t k[3] = {a, b, c};
        std::rotate(k, std::min_element(k, k + 3), k + 3);   // (same triangle, same winding: same key)
        uint64_t key = (uint64_t)k[0] * 0x9E3779B97F4A7C15ull ^ (uint64_t)k[1] * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)k[2];
        if (!seen.emplace(key, true).second) continue;
        outIndices.insert(outIndices.end(), {a, b, c});
    }
    // Only the vertices used
    std::vector<int64_t> used(outVertices.size(), -1);
    std::vector<Vertex> kept;
    for (uint32_t& i : outIndices) {
        if (used[i] < 0) {
            used[i] = (int64_t)kept.size();
            kept.push_back(outVertices[i]);
        }
        i = (uint32_t)used[i];
    }
    outVertices.swap(kept);
}

void generateLods(MeshLibrary& lib, int id) {
    if (id < 0 || id >= lib.count()) return;
    Mesh& mesh = lib.meshes[(size_t)id];
    mesh.lods.clear();
    uint32_t triangles = mesh.indexCount / 3;
    if (mesh.skinned || triangles < kLodMinTriangles) return;
    // The mesh's own vertices (the ones its triangles use) and triangles, numbered from 0
    std::unordered_map<uint32_t, uint32_t> local;
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    idx.reserve(mesh.indexCount);
    for (uint32_t k = 0; k < mesh.indexCount; ++k) {
        uint32_t global = (uint32_t)mesh.vertexOffset + lib.indices[mesh.firstIndex + k];
        auto [it, added] = local.try_emplace(global, (uint32_t)verts.size());
        if (added) verts.push_back(lib.vertices[global]);
        idx.push_back(it->second);
    }
    uint32_t previous = triangles;
    for (int level = 1; level <= kMaxLods; ++level) {
        // Aim for a quarter of the level before: search the grid size (fewer cells, fewer triangles)
        uint32_t target = std::max<uint32_t>(previous / 4, 12);
        int lo = 2, hi = std::max(4, (int)std::sqrt((float)previous) * 4);
        std::vector<Vertex> bestV, v;
        std::vector<uint32_t> bestI, i;
        for (int step = 0; step < 10 && lo <= hi; ++step) {
            int cells = (lo + hi) / 2;
            clusterSimplify(verts, idx, cells, v, i);
            uint32_t got = (uint32_t)i.size() / 3;
            if (bestI.empty() || std::abs((int)got - (int)target) < std::abs((int)bestI.size() / 3 - (int)target)) {
                bestV = v;
                bestI = i;
            }
            if (got > target) hi = cells - 1;
            else lo = cells + 1;
        }
        uint32_t got = (uint32_t)bestI.size() / 3;
        if (got == 0 || got > previous * 7 / 10) break;   // (barely simpler: not worth a level)
        MeshLod lod;
        lod.firstIndex = (uint32_t)lib.indices.size();
        lod.indexCount = (uint32_t)bestI.size();
        lod.vertexOffset = (int32_t)lib.vertices.size();
        lib.vertices.insert(lib.vertices.end(), bestV.begin(), bestV.end());
        lib.skin.resize(lib.vertices.size());
        lib.indices.insert(lib.indices.end(), bestI.begin(), bestI.end());
        lib.meshes[(size_t)id].lods.push_back(lod);
        previous = got;
    }
    ++lib.version;
}

int chooseLod(float radius, float distance, float tanHalfFov, int levels) {
    if (levels <= 0) return 0;
    float size = radius / std::max(distance * tanHalfFov, 1e-6f);
    int lod = size < kLod2Size ? 2 : size < kLod1Size ? 1 : 0;
    return std::min(lod, levels);
}
