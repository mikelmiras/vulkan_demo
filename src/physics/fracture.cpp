#include "physics/fracture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fracture {

namespace {

Vertex lerpVertex(const Vertex& a, const Vertex& b, float t) {
    Vertex v;
    v.pos = a.pos + (b.pos - a.pos) * t;
    v.normal = normalize(a.normal + (b.normal - a.normal) * t);
    v.u = a.u + (b.u - a.u) * t;
    v.v = a.v + (b.v - a.v) * t;
    return v;
}

void centreOf(Piece& p) {
    Vec3 sum;
    for (const Vertex& v : p.vertices) sum = sum + v.pos;
    p.centre = p.vertices.empty() ? Vec3{} : sum * (1.f / (float)p.vertices.size());
}

// The cut's outline (points on the plane) closed with a fan of triangles facing `facing`
void cap(Piece& p, std::vector<Vec3> points, Vec3 facing) {
    std::vector<Vec3> unique;   // (each crossing point appears twice: once per triangle sharing the edge)
    for (const Vec3& q : points)
        if (std::none_of(unique.begin(), unique.end(), [&](const Vec3& u) { return length(u - q) < 1e-5f; })) unique.push_back(q);
    if (unique.size() < 3) return;
    Vec3 c;
    for (const Vec3& q : unique) c = c + q;
    c = c * (1.f / (float)unique.size());
    Vec3 u = normalize(std::fabs(facing.y) < 0.9f ? cross(facing, {0, 1, 0}) : cross(facing, {1, 0, 0}));
    Vec3 w = cross(facing, u);
    std::sort(unique.begin(), unique.end(), [&](const Vec3& a, const Vec3& b) {
        return std::atan2(dot(a - c, w), dot(a - c, u)) < std::atan2(dot(b - c, w), dot(b - c, u));
    });
    auto vertexAt = [&](Vec3 q) {
        Vertex v;
        v.pos = q;
        v.normal = facing;
        v.u = dot(q, u);   // (flat mapping across the cut)
        v.v = dot(q, w);
        return v;
    };
    for (size_t k = 0; k < unique.size(); ++k) {
        Vec3 a = c, b = unique[k], d = unique[(k + 1) % unique.size()];
        if (dot(cross(b - a, d - a), facing) < 0.f) std::swap(b, d);   // (wound to face out)
        p.vertices.push_back(vertexAt(a));
        p.vertices.push_back(vertexAt(b));
        p.vertices.push_back(vertexAt(d));
    }
}

}  // namespace

std::pair<Piece, Piece> split(const Piece& piece, Vec3 n, float d) {
    Piece front, back;
    std::vector<Vec3> cutPoints;
    for (size_t t = 0; t + 2 < piece.vertices.size(); t += 3) {
        const Vertex* v[3] = {&piece.vertices[t], &piece.vertices[t + 1], &piece.vertices[t + 2]};
        float s[3];
        for (int k = 0; k < 3; ++k) s[k] = dot(n, v[k]->pos) - d;
        bool anyFront = s[0] > 1e-6f || s[1] > 1e-6f || s[2] > 1e-6f, anyBack = s[0] < -1e-6f || s[1] < -1e-6f || s[2] < -1e-6f;
        if (!anyBack) {
            front.vertices.insert(front.vertices.end(), {*v[0], *v[1], *v[2]});
            continue;
        }
        if (!anyFront) {
            back.vertices.insert(back.vertices.end(), {*v[0], *v[1], *v[2]});
            continue;
        }
        // Crossing: walk the triangle's outline, giving each side its corners and the crossings
        std::vector<Vertex> f, b;
        for (int k = 0; k < 3; ++k) {
            const Vertex &a = *v[k], &c = *v[(k + 1) % 3];
            float sa = s[k], sc = s[(k + 1) % 3];
            if (sa >= 0.f) f.push_back(a);
            if (sa <= 0.f) b.push_back(a);
            if (sa == 0.f) cutPoints.push_back(a.pos);   // (a corner on the plane: on the cut's outline)
            if ((sa > 0.f && sc < 0.f) || (sa < 0.f && sc > 0.f)) {
                Vertex x = lerpVertex(a, c, sa / (sa - sc));
                f.push_back(x);
                b.push_back(x);
                cutPoints.push_back(x.pos);
            }
        }
        for (size_t k = 1; k + 1 < f.size(); ++k) front.vertices.insert(front.vertices.end(), {f[0], f[k], f[k + 1]});
        for (size_t k = 1; k + 1 < b.size(); ++k) back.vertices.insert(back.vertices.end(), {b[0], b[k], b[k + 1]});
    }
    if (!front.vertices.empty() && !back.vertices.empty()) {
        cap(front, cutPoints, n * -1.f);   // (the front piece's cut faces back across the plane)
        cap(back, cutPoints, n);
    }
    centreOf(front);
    centreOf(back);
    return {front, back};
}

std::vector<Piece> cut(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, int count, uint32_t seed) {
    Piece whole;
    for (uint32_t i : indices)
        if (i < vertices.size()) whole.vertices.push_back(vertices[i]);
    centreOf(whole);
    std::vector<Piece> pieces{whole};
    uint32_t rng = seed * 2654435761u + 12345u;
    auto rand01 = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float)(rng & 0xFFFFFF) / (float)0x1000000;
    };
    auto size = [](const Piece& p) {
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const Vertex& v : p.vertices) {
            lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
            hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
        }
        Vec3 e = hi - lo;
        return e.x * e.y * e.z + 1e-9f * (e.x + e.y + e.z);
    };
    count = std::clamp(count, 1, 64);
    for (int guard = 0; (int)pieces.size() < count && guard < count * 8; ++guard) {
        // The biggest piece, through its centre, in a random direction (a little off it, so cuts don't all meet)
        size_t big = 0;
        for (size_t k = 1; k < pieces.size(); ++k)
            if (size(pieces[k]) > size(pieces[big])) big = k;
        Vec3 n = normalize(Vec3{rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f} + Vec3{1e-4f, 0.f, 0.f});
        Vec3 through = pieces[big].centre + Vec3{rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f} * 0.1f * std::cbrt(size(pieces[big]));
        auto [a, b] = split(pieces[big], n, dot(n, through));
        if (a.vertices.size() < 12 || b.vertices.size() < 12) continue;   // (a sliver: try another cut)
        pieces[big] = std::move(a);
        pieces.push_back(std::move(b));
    }
    return pieces;
}

void syncMeshes(Scene& scene, MeshLibrary& meshes) {
    for (Entity& e : scene.entities) {
        if (!e.fracture || !e.meshRenderer) continue;
        Fracture& f = *e.fracture;
        int mesh = e.meshRenderer->mesh;
        if (mesh < 0 || mesh >= meshes.count()) continue;
        uint64_t key = ((uint64_t)(uint32_t)mesh << 40) ^ ((uint64_t)(uint32_t)std::clamp(f.pieces, 1, 64) << 32) ^ f.seed;
        bool valid = f.piecesKey == key && !f.pieceMeshes.empty();
        for (int id : f.pieceMeshes) valid = valid && id >= 0 && id < meshes.count();
        if (valid) continue;
        // Made once per (mesh, count, seed): found by name when another entity (or an earlier play) made them
        char base[64];
        std::snprintf(base, sizeof(base), "Fracture %llx ", (unsigned long long)key);
        f.pieceMeshes.clear();
        for (int k = 0; meshes.find(base + std::to_string(k)) >= 0; ++k) f.pieceMeshes.push_back(meshes.find(base + std::to_string(k)));
        if (f.pieceMeshes.empty()) {
            const Mesh& m = meshes.get(mesh);
            std::vector<Vertex> verts;
            std::vector<uint32_t> idx;
            for (uint32_t k = 0; k < m.indexCount; ++k) {
                idx.push_back((uint32_t)verts.size());
                verts.push_back(meshes.vertices[(size_t)m.vertexOffset + meshes.indices[m.firstIndex + k]]);
            }
            std::vector<Piece> pieces = cut(verts, idx, f.pieces, f.seed);
            for (size_t k = 0; k < pieces.size(); ++k) {
                std::vector<uint32_t> order(pieces[k].vertices.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = (uint32_t)i;
                f.pieceMeshes.push_back(addMesh(meshes, base + std::to_string(k), "", pieces[k].vertices, order));
            }
        }
        f.piecesKey = key;
    }
}

}  // namespace fracture
