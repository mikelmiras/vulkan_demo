// Levels of detail (assets/mesh_lod.h): vertex clustering, the levels made at import, and which one to draw
#include "test.h"

#include <cmath>

#include "assets/mesh_lod.h"

namespace {

// A UV sphere of radius 1 with `rings` x `segments` quads (two triangles each)
int addSphere(MeshLibrary& lib, int rings, int segments) {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= segments; ++s) {
            float th = PI * (float)r / (float)rings, ph = 2.f * PI * (float)s / (float)segments;
            Vec3 p{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            verts.push_back({p, p, (float)s / (float)segments, (float)r / (float)rings});
        }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            uint32_t a = (uint32_t)(r * (segments + 1) + s), b = a + (uint32_t)segments + 1;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return addMesh(lib, "Ball", "", verts, idx);
}

}  // namespace

TEST(lod_levels_made_for_big_meshes) {
    MeshLibrary lib = buildMeshLibrary();
    int ball = addSphere(lib, 80, 80);   // 12800 triangles
    CHECK(lib.get(ball).lods.empty());   // (addMesh itself makes none: importers ask for them)
    generateLods(lib, ball);
    const Mesh& m = lib.get(ball);
    REQUIRE(m.lods.size() == 2);
    uint32_t full = m.indexCount / 3, l1 = m.lods[0].indexCount / 3, l2 = m.lods[1].indexCount / 3;
    CHECK(l1 > full / 8 && l1 < full / 2);   // about a quarter ...
    CHECK(l2 > l1 / 8 && l2 < l1 / 2);      // ... of the one before
    // Every level is still the sphere: corners near radius 1, normals pointing out, indices in range
    for (const MeshLod& lod : m.lods) {
        uint32_t maxIndex = 0;
        for (uint32_t k = 0; k < lod.indexCount; ++k) maxIndex = std::max(maxIndex, lib.indices[lod.firstIndex + k]);
        REQUIRE((size_t)lod.vertexOffset + maxIndex < lib.vertices.size());
        for (uint32_t k = 0; k < lod.indexCount; ++k) {
            const Vertex& v = lib.vertices[(size_t)lod.vertexOffset + lib.indices[lod.firstIndex + k]];
            CHECK(length(v.pos) > 0.85f && length(v.pos) < 1.01f);
            CHECK(dot(v.normal, normalize(v.pos)) > 0.7f);
        }
    }
    // Small meshes, and skinned ones, get none
    int cube = (int)MeshKind::Cube;
    generateLods(lib, cube);
    CHECK(lib.get(cube).lods.empty());
    lib.meshes[(size_t)ball].skinned = true;
    generateLods(lib, ball);
    CHECK(lib.get(ball).lods.empty());
}

TEST(lod_clustering_keeps_texture_seams) {
    // Two quads side by side sharing an edge in space but not in texture space (u 0..0.2 and 0.8..1): clustering
    // merges each side's corners, never across the seam
    std::vector<Vertex> verts = {
        {{0, 0, 0}, {0, 0, 1}, 0.f, 0.f},   {{0.01f, 0, 0}, {0, 0, 1}, 0.2f, 0.f},
        {{0, 1, 0}, {0, 0, 1}, 0.f, 1.f},   {{0.01f, 1, 0}, {0, 0, 1}, 0.2f, 1.f},
        {{0.01f, 0, 0}, {0, 0, 1}, 0.9f, 0.f}, {{1, 0, 0}, {0, 0, 1}, 1.f, 0.f},
        {{0.01f, 1, 0}, {0, 0, 1}, 0.9f, 1.f}, {{1, 1, 0}, {0, 0, 1}, 1.f, 1.f}};
    std::vector<uint32_t> idx = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    std::vector<Vertex> out;
    std::vector<uint32_t> outIdx;
    clusterSimplify(verts, idx, 4, out, outIdx);
    for (const Vertex& v : out) CHECK(v.u < 0.25f || v.u > 0.85f);   // (no corner averaged across the seam)
    CHECK(outIdx.size() == 6);   // the thin quad collapsed; the wide one stayed
}

TEST(lod_choice_by_screen_size) {
    float t = std::tan(30.f * DEG2RAD);
    CHECK(chooseLod(1.f, 2.f, t, 2) == 0);     // close: the full mesh
    CHECK(chooseLod(1.f, 20.f, t, 2) == 1);    // (1 / (20 * 0.577) = 0.087)
    CHECK(chooseLod(1.f, 100.f, t, 2) == 2);
    CHECK(chooseLod(1.f, 100.f, t, 1) == 1);   // never more levels than it has
    CHECK(chooseLod(1.f, 100.f, t, 0) == 0);
}
