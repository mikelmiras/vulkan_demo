#include "physics/fracture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

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

struct Segment {
    Vec3 a, b;
};

// Where edge a-c crosses the plane (n . p = d), worked out the same way from either end (the two triangles sharing the
// edge get the very same point, so their cut outlines meet exactly)
Vertex crossing(const Vertex& a, const Vertex& c, Vec3 n, float d) {
    auto before = [](Vec3 p, Vec3 q) { return p.x != q.x ? p.x < q.x : p.y != q.y ? p.y < q.y : p.z < q.z; };
    const Vertex &p = before(a.pos, c.pos) ? a : c, &q = before(a.pos, c.pos) ? c : a;
    float sp = dot(n, p.pos) - d, sq = dot(n, q.pos) - d;
    return lerpVertex(p, q, sp / (sp - sq));
}

// A cut piece's edges that only one of its triangles has, lying on the plane: the outline its cap closes
std::vector<Segment> openEdges(const Piece& p, Vec3 n, float d, float eps) {
    std::map<std::tuple<float, float, float, float, float, float>, int> edges;   // (a -> b: +1, b -> a: -1)
    auto key = [](Vec3 a, Vec3 b) { return std::make_tuple(a.x, a.y, a.z, b.x, b.y, b.z); };
    auto before = [](Vec3 a, Vec3 b) { return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z; };
    for (size_t t = 0; t + 2 < p.vertices.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
            Vec3 a = p.vertices[t + (size_t)k].pos, b = p.vertices[t + (size_t)(k + 1) % 3].pos;
            if (std::fabs(dot(n, a) - d) > eps || std::fabs(dot(n, b) - d) > eps) continue;
            if (before(a, b)) edges[key(a, b)] += 1;
            else edges[key(b, a)] -= 1;
        }
    std::vector<Segment> out;
    for (const auto& [k, count] : edges)
        if (count != 0) out.push_back({{std::get<0>(k), std::get<1>(k), std::get<2>(k)}, {std::get<3>(k), std::get<4>(k), std::get<5>(k)}});
    return out;
}

void bounds(const Piece& p, Vec3& lo, Vec3& hi) {
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : p.vertices) {
        lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
        hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
    }
}

void centreOf(Piece& p) {
    Vec3 sum;
    for (const Vertex& v : p.vertices) sum = sum + v.pos;
    p.centre = p.vertices.empty() ? Vec3{} : sum * (1.f / (float)p.vertices.size());
}

// The cut's outline closed with flat triangles facing `facing`: its segments joined into loops, loops inside others
// made holes (bridged into them), each outline ear-clipped (concave ones too)

void cap(Piece& p, std::vector<Segment> segments, Vec3 facing) {
    const float eps = 1e-5f;
    for (size_t i = 0; i < segments.size(); ++i)   // (an edge on the plane, shared by two triangles: once)
        for (size_t j = segments.size(); j-- > i + 1;) {
            const Segment &x = segments[i], &y = segments[j];
            if ((length(x.a - y.a) < eps && length(x.b - y.b) < eps) || (length(x.a - y.b) < eps && length(x.b - y.a) < eps))
                segments.erase(segments.begin() + (long)j);
        }
    Vec3 u = normalize(std::fabs(facing.y) < 0.9f ? cross(facing, {0, 1, 0}) : cross(facing, {1, 0, 0}));
    Vec3 w = cross(facing, u);   // (u x w = facing: counter-clockwise in (u, w) faces out)
    // Loops: segments chained end to start (each crossing edge gave its two triangles the same point)
    std::vector<bool> used(segments.size(), false);
    std::vector<std::vector<Vec3>> loops;
    for (size_t s0 = 0; s0 < segments.size(); ++s0) {
        if (used[s0] || length(segments[s0].a - segments[s0].b) < eps) continue;
        used[s0] = true;
        std::vector<Vec3> loop{segments[s0].a, segments[s0].b};
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t s = 0; s < segments.size(); ++s) {
                if (used[s]) continue;
                Vec3 end = loop.back(), next;
                if (length(segments[s].a - end) < eps) next = segments[s].b;
                else if (length(segments[s].b - end) < eps) next = segments[s].a;
                else continue;
                used[s] = true;
                grew = true;
                if (length(next - loop.front()) < eps) break;   // (closed)
                loop.push_back(next);
            }
            if (loop.size() > 2 && length(loop.back() - loop.front()) < eps) loop.pop_back();
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    if (loops.empty()) return;
    using P2 = std::pair<float, float>;
    auto to2 = [&](Vec3 q) { return P2{dot(q, u), dot(q, w)}; };
    auto area = [&](const std::vector<Vec3>& l) {
        float a = 0.f;
        for (size_t i = 0; i < l.size(); ++i) {
            P2 p0 = to2(l[i]), p1 = to2(l[(i + 1) % l.size()]);
            a += p0.first * p1.second - p1.first * p0.second;
        }
        return a * 0.5f;
    };
    auto inside = [&](P2 q, const std::vector<Vec3>& l) {   // (even-odd)
        bool in = false;
        for (size_t i = 0, j = l.size() - 1; i < l.size(); j = i++) {
            P2 a = to2(l[i]), b = to2(l[j]);
            if ((a.second > q.second) != (b.second > q.second) &&
                q.first < (b.first - a.first) * (q.second - a.second) / (b.second - a.second) + a.first)
                in = !in;
        }
        return in;
    };
    // Outer loops counter-clockwise, holes (inside an odd number of others) clockwise, each bridged into its outer
    std::vector<int> depth(loops.size(), 0);
    for (size_t i = 0; i < loops.size(); ++i)
        for (size_t j = 0; j < loops.size(); ++j)
            if (i != j && inside(to2(loops[i][0]), loops[j])) ++depth[i];
    std::vector<std::vector<Vec3>> outlines;
    std::vector<size_t> outerOf(loops.size(), (size_t)-1);
    for (size_t i = 0; i < loops.size(); ++i) {
        bool hole = depth[i] % 2 == 1;
        if ((area(loops[i]) > 0.f) == hole) std::reverse(loops[i].begin(), loops[i].end());
        if (!hole) {
            outerOf[i] = outlines.size();
            outlines.push_back(loops[i]);
        }
    }
    for (size_t h = 0; h < loops.size(); ++h) {
        if (depth[h] % 2 == 0) continue;
        // Its outer loop: the deepest even one holding it
        size_t best = (size_t)-1;
        for (size_t o = 0; o < loops.size(); ++o)
            if (depth[o] % 2 == 0 && depth[o] == depth[h] - 1 && inside(to2(loops[h][0]), loops[o])) best = o;
        if (best == (size_t)-1 || outerOf[best] == (size_t)-1) continue;
        std::vector<Vec3>& outer = outlines[outerOf[best]];
        const std::vector<Vec3>& hole = loops[h];
        size_t hi = 0;   // the hole's rightmost point, bridged to the nearest outer point to its right
        for (size_t k = 1; k < hole.size(); ++k)
            if (to2(hole[k]).first > to2(hole[hi]).first) hi = k;
        P2 hp = to2(hole[hi]);
        size_t oi = 0;
        float bestD = 1e30f;
        for (size_t k = 0; k < outer.size(); ++k) {
            P2 q = to2(outer[k]);
            float d = (q.first - hp.first) * (q.first - hp.first) + (q.second - hp.second) * (q.second - hp.second);
            if (q.first >= hp.first && d < bestD) bestD = d, oi = k;
        }
        std::vector<Vec3> merged(outer.begin(), outer.begin() + (long)oi + 1);
        for (size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(hi + k) % hole.size()]);
        merged.push_back(outer[oi]);
        merged.insert(merged.end(), outer.begin() + (long)oi + 1, outer.end());
        outer = std::move(merged);
    }
    auto vertexAt = [&](Vec3 q) {
        Vertex v;
        v.pos = q;
        v.normal = facing;
        v.u = dot(q, u);   // (flat mapping across the cut)
        v.v = dot(q, w);
        return v;
    };
    // Ear clipping: a corner that turns left with no other point in its triangle is cut off, until three are left
    for (std::vector<Vec3>& poly : outlines) {
        std::vector<Vec3> rest = poly;
        auto cross2 = [&](P2 a, P2 b, P2 c) { return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first); };
        for (int guard = 0; rest.size() > 3 && guard < 10000; ++guard) {
            bool clipped = false;
            for (size_t i = 0; i < rest.size() && !clipped; ++i) {
                size_t ip = (i + rest.size() - 1) % rest.size(), in = (i + 1) % rest.size();
                P2 a = to2(rest[ip]), b = to2(rest[i]), c = to2(rest[in]);
                if (cross2(a, b, c) <= 1e-12f) continue;   // (reflex or flat)
                bool empty = true;
                for (size_t k = 0; k < rest.size() && empty; ++k) {
                    if (k == ip || k == i || k == in) continue;
                    P2 q = to2(rest[k]);
                    auto same = [&](P2 r) { return std::fabs(q.first - r.first) < 1e-7f && std::fabs(q.second - r.second) < 1e-7f; };
                    if (same(a) || same(b) || same(c)) continue;   // (a bridge's twin of a corner)
                    if (cross2(a, b, q) >= 0.f && cross2(b, c, q) >= 0.f && cross2(c, a, q) >= 0.f) empty = false;
                }
                if (!empty) continue;
                p.vertices.push_back(vertexAt(rest[ip]));
                p.vertices.push_back(vertexAt(rest[i]));
                p.vertices.push_back(vertexAt(rest[in]));
                rest.erase(rest.begin() + (long)i);
                clipped = true;
            }
            if (!clipped) break;   // (degenerate: what's left stays open)
        }
        if (rest.size() == 3 && cross2(to2(rest[0]), to2(rest[1]), to2(rest[2])) > 0.f)
            for (const Vec3& q : rest) p.vertices.push_back(vertexAt(q));
    }
}

}  // namespace

std::pair<Piece, Piece> split(const Piece& piece, Vec3 n, float d) {
    Piece front, back;
    for (size_t t = 0; t + 2 < piece.vertices.size(); t += 3) {
        const Vertex* v[3] = {&piece.vertices[t], &piece.vertices[t + 1], &piece.vertices[t + 2]};
        float s[3];
        for (int k = 0; k < 3; ++k) s[k] = dot(n, v[k]->pos) - d;
        bool anyFront = s[0] > 1e-6f || s[1] > 1e-6f || s[2] > 1e-6f, anyBack = s[0] < -1e-6f || s[1] < -1e-6f || s[2] < -1e-6f;
        if (!anyBack && !anyFront) {   // (in the plane: a face of the side behind it)
            bool facesFront = dot(cross(v[1]->pos - v[0]->pos, v[2]->pos - v[0]->pos), n) > 0.f;
            (facesFront ? back : front).vertices.insert((facesFront ? back : front).vertices.end(), {*v[0], *v[1], *v[2]});
            continue;
        }
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
            if ((sa > 0.f && sc < 0.f) || (sa < 0.f && sc > 0.f)) {
                Vertex x = crossing(a, c, n, d);
                f.push_back(x);
                b.push_back(x);
            }
        }
        for (size_t k = 1; k + 1 < f.size(); ++k) front.vertices.insert(front.vertices.end(), {f[0], f[k], f[k + 1]});
        for (size_t k = 1; k + 1 < b.size(); ++k) back.vertices.insert(back.vertices.end(), {b[0], b[k], b[k + 1]});
    }
    if (!front.vertices.empty() && !back.vertices.empty()) {
        // (each side capped along its own open outline: faces lying in the plane leave a side less to close)
        Vec3 lo, hi;
        bounds(piece, lo, hi);
        float eps = std::max(length(hi - lo), 1e-3f) * 1e-5f;
        cap(front, openEdges(front, n, d, eps), n * -1.f);   // (the front piece's cut faces back across the plane)
        cap(back, openEdges(back, n, d, eps), n);
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

bool isConvex(const Piece& piece, float tolerance) {
    Vec3 lo, hi;
    bounds(piece, lo, hi);
    float eps = tolerance * std::max(length(hi - lo), 1e-6f);
    // (big pieces: a sample of the planes and corners, at most about two thousand of each)
    size_t triangles = piece.vertices.size() / 3;
    size_t planeStep = std::max<size_t>(1, triangles / 2000), cornerStep = std::max<size_t>(1, piece.vertices.size() / 2000);
    for (size_t t = 0; t + 2 < piece.vertices.size(); t += 3 * planeStep) {
        Vec3 a = piece.vertices[t].pos, b = piece.vertices[t + 1].pos, c = piece.vertices[t + 2].pos;
        Vec3 n = cross(b - a, c - a);
        if (length(n) < 1e-12f) continue;
        n = normalize(n);
        for (size_t k = 0; k < piece.vertices.size(); k += cornerStep)
            if (dot(n, piece.vertices[k].pos - a) > eps) return false;
    }
    return true;
}

std::vector<Piece> convexParts(const Piece& piece, int maxParts) {
    std::vector<Piece> parts{piece};
    std::vector<bool> done{false};
    while ((int)parts.size() < maxParts) {
        size_t pick = parts.size();
        float biggest = -1.f;
        for (size_t k = 0; k < parts.size(); ++k) {
            if (done[k]) continue;
            if (isConvex(parts[k])) {
                done[k] = true;
                continue;
            }
            Vec3 lo, hi;
            bounds(parts[k], lo, hi);
            float size = length(hi - lo);
            if (size > biggest) biggest = size, pick = k;
        }
        if (pick == parts.size()) break;   // (all convex)
        Vec3 lo, hi;
        bounds(parts[pick], lo, hi);
        Vec3 e = hi - lo, mid = (lo + hi) * 0.5f;
        Vec3 n = e.x >= e.y && e.x >= e.z ? Vec3{1, 0, 0} : e.y >= e.z ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
        auto [a, b] = split(parts[pick], n, dot(n, mid));
        if (a.vertices.size() < 12 || b.vertices.size() < 12) {   // (couldn't: kept whole)
            done[pick] = true;
            continue;
        }
        parts[pick] = std::move(a);
        parts.push_back(std::move(b));
        done.push_back(false);
    }
    return parts;
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
            auto add = [&](const std::string& name, const Piece& p) {
                std::vector<uint32_t> order(p.vertices.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = (uint32_t)i;
                return addMesh(meshes, name, "", p.vertices, order);
            };
            for (size_t k = 0; k < pieces.size(); ++k) {
                int id = add(base + std::to_string(k), pieces[k]);
                f.pieceMeshes.push_back(id);
                std::vector<Piece> parts = convexParts(pieces[k]);   // (a concave piece: its hulls together)
                if (parts.size() > 1) {
                    std::vector<int> partMeshes;
                    for (size_t j = 0; j < parts.size(); ++j)
                        partMeshes.push_back(add(base + std::to_string(k) + " part " + std::to_string(j), parts[j]));
                    meshes.meshes[(size_t)id].convexParts = partMeshes;
                }
            }
        }
        f.piecesKey = key;
    }
}

}  // namespace fracture
