#include "geometry.h"

#include <utility>

const char* meshKindName(MeshKind kind) {
    static const char* names[] = {"Plane", "Cube", "Sphere", "Pyramid", "Torus"};
    return names[(int)kind];
}

namespace {

struct Builder {
    MeshLibrary& lib;
    Mesh current{};

    void begin() { current = {(uint32_t)lib.indices.size(), 0, (int32_t)lib.vertices.size()}; }
    Mesh end() { current.indexCount = (uint32_t)lib.indices.size() - current.firstIndex; return current; }
    uint32_t base() const { return (uint32_t)lib.vertices.size() - (uint32_t)current.vertexOffset; }

    // Flat-shaded triangle, wound counter-clockwise when seen from outside (away from `inside`)
    void flatTri(Vec3 a, Vec3 b, Vec3 c, Vec3 inside) {
        Vec3 n = normalize(cross(b - a, c - a));
        if (dot(n, a - inside) < 0.f) { std::swap(b, c); n = n * -1.f; }
        uint32_t i = base();
        lib.vertices.push_back({a, n}); lib.vertices.push_back({b, n}); lib.vertices.push_back({c, n});
        lib.indices.insert(lib.indices.end(), {i, i + 1, i + 2});
    }

    // Smooth parametric surface: grid of (rows+1)x(cols+1) vertices from fn(u,v) -> Vertex
    template <class F>
    void grid(int rows, int cols, F fn) {
        uint32_t b = base();
        for (int i = 0; i <= rows; ++i)
            for (int j = 0; j <= cols; ++j) lib.vertices.push_back(fn((float)i / rows, (float)j / cols));
        auto idx = [&](int i, int j) { return b + (uint32_t)(i * (cols + 1) + j); };
        auto tri = [&](uint32_t a, uint32_t c, uint32_t d) {
            const Vertex* v = &lib.vertices[current.vertexOffset];
            Vec3 n = cross(v[c].pos - v[a].pos, v[d].pos - v[a].pos);
            Vec3 avg = v[a].normal + v[c].normal + v[d].normal;
            if (dot(n, avg) < 0.f) std::swap(c, d);   // keep CCW-from-outside winding
            lib.indices.insert(lib.indices.end(), {a, c, d});
        };
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) {
                tri(idx(i, j), idx(i + 1, j), idx(i + 1, j + 1));
                tri(idx(i, j), idx(i + 1, j + 1), idx(i, j + 1));
            }
    }
};

}  // namespace

MeshLibrary buildMeshLibrary() {
    MeshLibrary lib;
    Builder g{lib};
    const Vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1}, O{0, 0, 0};

    // Unit plane on XZ, facing +Y
    g.begin();
    Vec3 p0{-.5f, 0, -.5f}, p1{.5f, 0, -.5f}, p2{.5f, 0, .5f}, p3{-.5f, 0, .5f};
    g.flatTri(p0, p1, p2, {0, -1, 0});
    g.flatTri(p0, p2, p3, {0, -1, 0});
    lib.meshes[(int)MeshKind::Plane] = g.end();

    // Unit cube centred at the origin
    g.begin();
    Vec3 axes[3] = {X, Y, Z};
    for (int a = 0; a < 3; ++a)
        for (float sgn : {1.f, -1.f}) {
            Vec3 n = axes[a] * sgn, u = axes[(a + 1) % 3] * 0.5f, v = axes[(a + 2) % 3] * 0.5f;
            Vec3 c = n * 0.5f;
            g.flatTri(c - u - v, c + u - v, c + u + v, O);
            g.flatTri(c - u - v, c + u + v, c - u + v, O);
        }
    lib.meshes[(int)MeshKind::Cube] = g.end();

    // Sphere, radius 0.5
    g.begin();
    g.grid(24, 48, [](float u, float v) {
        float th = u * PI, ph = v * 2.f * PI;
        Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
        return Vertex{n * 0.5f, n};
    });
    lib.meshes[(int)MeshKind::Sphere] = g.end();

    // Square pyramid, base on y=0, apex at y=1
    g.begin();
    Vec3 apex{0, 1, 0}, inside{0, 0.3f, 0};
    Vec3 b[4] = {{-.5f, 0, -.5f}, {.5f, 0, -.5f}, {.5f, 0, .5f}, {-.5f, 0, .5f}};
    for (int i = 0; i < 4; ++i) g.flatTri(b[i], b[(i + 1) % 4], apex, inside);
    g.flatTri(b[0], b[1], b[2], inside);
    g.flatTri(b[0], b[2], b[3], inside);
    lib.meshes[(int)MeshKind::Pyramid] = g.end();

    // Torus around Y, outer radius 0.5
    g.begin();
    g.grid(24, 48, [](float u, float v) {
        const float R = 0.35f, r = 0.15f;
        float a = v * 2.f * PI, b2 = u * 2.f * PI;
        Vec3 ring{std::cos(a), 0, std::sin(a)};
        Vec3 n = ring * std::cos(b2) + Vec3{0, std::sin(b2), 0};
        return Vertex{ring * R + n * r, n};
    });
    lib.meshes[(int)MeshKind::Torus] = g.end();
    return lib;
}

float intersectMesh(const MeshLibrary& lib, MeshKind kind, Vec3 origin, Vec3 dir) {
    // Every mesh fits in the [-0.5, 1]^3 box: cheap slab test first
    float tmin = -1e30f, tmax = 1e30f;
    const float lo[3] = {-0.51f, -0.51f, -0.51f}, hi[3] = {0.51f, 1.01f, 0.51f};
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return -1.f;
            continue;
        }
        float t0 = (lo[i] - o[i]) / d[i], t1 = (hi[i] - o[i]) / d[i];
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
    }
    if (tmax < std::max(tmin, 0.f)) return -1.f;

    // Möller–Trumbore against every triangle (double-sided)
    const Mesh& m = lib.get(kind);
    const Vertex* v = &lib.vertices[m.vertexOffset];
    float best = -1.f;
    for (uint32_t i = 0; i < m.indexCount; i += 3) {
        Vec3 a = v[lib.indices[m.firstIndex + i]].pos;
        Vec3 b = v[lib.indices[m.firstIndex + i + 1]].pos;
        Vec3 c = v[lib.indices[m.firstIndex + i + 2]].pos;
        Vec3 e1 = b - a, e2 = c - a, p = cross(dir, e2);
        float det = dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        float inv = 1.f / det;
        Vec3 s = origin - a;
        float u = dot(s, p) * inv;
        if (u < 0.f || u > 1.f) continue;
        Vec3 q = cross(s, e1);
        float w = dot(dir, q) * inv;
        if (w < 0.f || u + w > 1.f) continue;
        float t = dot(e2, q) * inv;
        if (t > 0.f && (best < 0.f || t < best)) best = t;
    }
    return best;
}
