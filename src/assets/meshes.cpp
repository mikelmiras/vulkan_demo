#include "assets/meshes.h"

#include <tuple>

#include "assets/gltf.h"

#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

const AnimationClip* MeshLibrary::findAnimation(const std::string& name) const {
    for (const AnimationClip& a : animations)
        if (a.name == name) return &a;
    return nullptr;
}

int addSkinnedMesh(MeshLibrary& lib, const std::string& name, const std::string& source,
                   const std::vector<Vertex>& vertices, const std::vector<SkinVertex>& skin,
                   const std::vector<uint32_t>& indices) {
    size_t first = lib.vertices.size();
    int id = addMesh(lib, name, source, vertices, indices);
    for (size_t i = 0; i < skin.size() && i < vertices.size(); ++i) lib.skin[first + i] = skin[i];
    lib.meshes[(size_t)id].skinned = true;
    return id;
}

int MeshLibrary::find(const std::string& name) const {
    for (int i = 0; i < count(); ++i)
        if (meshes[(size_t)i].name == name) return i;
    return -1;
}

namespace {

void computeBounds(const MeshLibrary& lib, Mesh& m) {
    m.boundsMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    m.boundsMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = (size_t)m.vertexOffset; i < lib.vertices.size(); ++i) {
        const Vec3& p = lib.vertices[i].pos;
        m.boundsMin = {std::min(m.boundsMin.x, p.x), std::min(m.boundsMin.y, p.y), std::min(m.boundsMin.z, p.z)};
        m.boundsMax = {std::max(m.boundsMax.x, p.x), std::max(m.boundsMax.y, p.y), std::max(m.boundsMax.z, p.z)};
    }
}

struct Builder {
    MeshLibrary& lib;
    Mesh current{};

    void begin() { current = {(uint32_t)lib.indices.size(), 0, (int32_t)lib.vertices.size()}; }
    void end(const char* name) {
        current.indexCount = (uint32_t)lib.indices.size() - current.firstIndex;
        current.name = name;
        computeBounds(lib, current);
        lib.meshes.push_back(current);
    }
    uint32_t base() const { return (uint32_t)lib.vertices.size() - (uint32_t)current.vertexOffset; }

    // Flat-shaded triangle, wound counter-clockwise when seen from outside (away from `inside`).
    // Texture coordinates: projected along the normal's main axis (box mapping), one texture per unit.
    void flatTri(Vec3 a, Vec3 b, Vec3 c, Vec3 inside) {
        Vec3 n = normalize(cross(b - a, c - a));
        if (dot(n, a - inside) < 0.f) { std::swap(b, c); n = n * -1.f; }
        Vec3 t, bt;   // texture u and v directions on the face
        float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        if (ax >= ay && ax >= az) { t = {0, 0, n.x > 0.f ? -1.f : 1.f}; bt = {0, -1, 0}; }
        else if (ay >= az) { t = {1, 0, 0}; bt = {0, 0, n.y > 0.f ? 1.f : -1.f}; }
        else { t = {n.z > 0.f ? 1.f : -1.f, 0, 0}; bt = {0, -1, 0}; }
        uint32_t i = base();
        for (Vec3 p : {a, b, c}) lib.vertices.push_back({p, n, dot(p, t) + 0.5f, dot(p, bt) + 0.5f});
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
    g.end("Plane");

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
    g.end("Cube");

    // Sphere, radius 0.5
    g.begin();
    g.grid(24, 48, [](float u, float v) {
        float th = u * PI, ph = v * 2.f * PI;
        Vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
        return Vertex{n * 0.5f, n, v, u};   // u around, v from top to bottom
    });
    g.end("Sphere");

    // Square pyramid, base on y=0, apex at y=1
    g.begin();
    Vec3 apex{0, 1, 0}, inside{0, 0.3f, 0};
    Vec3 b[4] = {{-.5f, 0, -.5f}, {.5f, 0, -.5f}, {.5f, 0, .5f}, {-.5f, 0, .5f}};
    for (int i = 0; i < 4; ++i) g.flatTri(b[i], b[(i + 1) % 4], apex, inside);
    g.flatTri(b[0], b[1], b[2], inside);
    g.flatTri(b[0], b[2], b[3], inside);
    g.end("Pyramid");

    // Torus around Y, outer radius 0.5
    g.begin();
    g.grid(24, 48, [](float u, float v) {
        const float R = 0.35f, r = 0.15f;
        float a = v * 2.f * PI, b2 = u * 2.f * PI;
        Vec3 ring{std::cos(a), 0, std::sin(a)};
        Vec3 n = ring * std::cos(b2) + Vec3{0, std::sin(b2), 0};
        return Vertex{ring * R + n * r, n, v * 4.f, u};
    });
    g.end("Torus");
    lib.skin.resize(lib.vertices.size());   // built-ins aren't skinned
    return lib;
}

// ---------------------------------------------------------------------------------------------
// OBJ import
// ---------------------------------------------------------------------------------------------
namespace {

// OBJ indices are 1-based, or negative = relative to the end of the list so far. Returns 0-based or -1.
int resolveIndex(const char* s, char** end, size_t count) {
    long i = std::strtol(s, end, 10);
    if (*end == s) return -1;
    long r = i > 0 ? i - 1 : (long)count + i;
    return r >= 0 && r < (long)count ? (int)r : -1;
}

}  // namespace

int importObj(MeshLibrary& lib, const std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
    std::string source = (ec ? fs::path(path) : abs).u8string();
    for (int i = 0; i < lib.count(); ++i)
        if (lib.meshes[(size_t)i].source == source) return i;

    std::ifstream f(path);
    if (!f) { error = "cannot open " + path; return -1; }

    std::vector<Vec3> positions, normals;
    std::vector<std::pair<float, float>> texcoords;
    struct Corner { int v, n, t; };
    std::vector<Corner> tris;   // 3 corners per triangle
    std::vector<Corner> face;
    std::string line;
    while (std::getline(f, line)) {
        const char* p = line.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            Vec3 v;
            char* e;
            v.x = std::strtof(p + 2, &e); v.y = std::strtof(e, &e); v.z = std::strtof(e, &e);
            positions.push_back(v);
        } else if (p[0] == 'v' && p[1] == 't') {
            char* e;
            float u = std::strtof(p + 2, &e), v = std::strtof(e, &e);
            texcoords.emplace_back(u, 1.f - v);   // OBJ's v goes up, ours down
        } else if (p[0] == 'v' && p[1] == 'n') {
            Vec3 n;
            char* e;
            n.x = std::strtof(p + 2, &e); n.y = std::strtof(e, &e); n.z = std::strtof(e, &e);
            normals.push_back(n);
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            // f v  |  v/vt  |  v//vn  |  v/vt/vn
            face.clear();
            char* e = (char*)p + 1;
            for (;;) {
                while (*e == ' ' || *e == '\t') ++e;
                if (!*e || *e == '\r' || *e == '#') break;
                Corner c{resolveIndex(e, &e, positions.size()), -1, -1};
                if (c.v < 0) { error = path + ": bad face index"; return -1; }
                if (*e == '/') {
                    ++e;
                    if (*e != '/') c.t = resolveIndex(e, &e, texcoords.size());
                    if (*e == '/') { ++e; c.n = resolveIndex(e, &e, normals.size()); }
                }
                while (*e && *e != ' ' && *e != '\t') ++e;   // skip anything unexpected in this token
                face.push_back(c);
            }
            for (size_t i = 2; i < face.size(); ++i) tris.insert(tris.end(), {face[0], face[i - 1], face[i]});
        }
    }
    if (tris.empty()) { error = path + ": no faces found"; return -1; }

    // Recentre and scale to fit the unit cube (largest side = 1), like the built-in meshes
    Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const Corner& c : tris) {
        const Vec3& v = positions[(size_t)c.v];
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    Vec3 center = (lo + hi) * 0.5f;
    float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    float scale = extent > 0.f ? 1.f / extent : 1.f;

    Mesh m;
    m.firstIndex = (uint32_t)lib.indices.size();
    m.vertexOffset = (int32_t)lib.vertices.size();
    std::map<std::tuple<int, int, int>, uint32_t> shared;   // (position, normal, uv) -> vertex, for smooth corners
    for (size_t t = 0; t < tris.size(); t += 3) {
        Vec3 p[3];
        for (int k = 0; k < 3; ++k) p[k] = (positions[(size_t)tris[t + k].v] - center) * scale;
        Vec3 faceN = cross(p[1] - p[0], p[2] - p[0]);
        if (length(faceN) < 1e-12f) continue;   // degenerate
        faceN = normalize(faceN);
        for (int k = 0; k < 3; ++k) {
            const Corner& c = tris[t + k];
            float u = c.t >= 0 ? texcoords[(size_t)c.t].first : 0.f, v = c.t >= 0 ? texcoords[(size_t)c.t].second : 0.f;
            if (c.n >= 0) {
                auto [it, added] = shared.try_emplace({c.v, c.n, c.t}, (uint32_t)(lib.vertices.size() - (size_t)m.vertexOffset));
                if (added) lib.vertices.push_back({p[k], normalize(normals[(size_t)c.n]), u, v});
                lib.indices.push_back(it->second);
            } else {   // no normal in the file: flat shading
                lib.indices.push_back((uint32_t)(lib.vertices.size() - (size_t)m.vertexOffset));
                lib.vertices.push_back({p[k], faceN, u, v});
            }
        }
    }
    m.indexCount = (uint32_t)lib.indices.size() - m.firstIndex;
    if (m.indexCount == 0) { error = path + ": only degenerate faces"; return -1; }
    computeBounds(lib, m);

    std::string base = fs::path(path).stem().u8string(), name = base;
    for (int n = 2; lib.find(name) >= 0; ++n) name = base + " " + std::to_string(n);
    m.name = name;
    m.source = source;
    lib.meshes.push_back(m);
    ++lib.version;
    return lib.count() - 1;
}

int addMesh(MeshLibrary& lib, const std::string& name, const std::string& source, const std::vector<Vertex>& vertices,
            const std::vector<uint32_t>& indices) {
    Mesh m;
    m.firstIndex = (uint32_t)lib.indices.size();
    m.vertexOffset = (int32_t)lib.vertices.size();
    m.indexCount = (uint32_t)indices.size();
    lib.skin.resize(lib.vertices.size());   // (built-in meshes add vertices directly)
    lib.vertices.insert(lib.vertices.end(), vertices.begin(), vertices.end());
    lib.skin.resize(lib.vertices.size());
    lib.indices.insert(lib.indices.end(), indices.begin(), indices.end());
    computeBounds(lib, m);
    std::string unique = name;
    for (int n = 2; lib.find(unique) >= 0; ++n) unique = name + " " + std::to_string(n);
    m.name = unique;
    m.source = source;
    lib.meshes.push_back(m);
    ++lib.version;
    return lib.count() - 1;
}

int importMesh(MeshLibrary& lib, const std::string& source, std::string& error) {
    for (int i = 0; i < lib.count(); ++i)
        if (lib.meshes[(size_t)i].source == source) return i;
    size_t hash = source.rfind('#');
    if (hash == std::string::npos) return importObj(lib, source, error);
    // A glTF primitive: importing the file brings in all of its meshes, then pick this one
    if (!importGltfMeshes(lib, source.substr(0, hash), error)) return -1;
    for (int i = 0; i < lib.count(); ++i)
        if (lib.meshes[(size_t)i].source == source) return i;
    error = source + ": no such mesh in the file";
    return -1;
}

float intersectMesh(const MeshLibrary& lib, int mesh, Vec3 origin, Vec3 dir, Vec3* normal) {
    const Mesh& m = lib.get(mesh);
    // Cheap slab test against the (slightly padded) bounding box first
    float tmin = -1e30f, tmax = 1e30f;
    const float lo[3] = {m.boundsMin.x - 0.01f, m.boundsMin.y - 0.01f, m.boundsMin.z - 0.01f};
    const float hi[3] = {m.boundsMax.x + 0.01f, m.boundsMax.y + 0.01f, m.boundsMax.z + 0.01f};
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
        if (t > 0.f && (best < 0.f || t < best)) {
            best = t;
            if (normal) *normal = normalize(cross(e1, e2));
        }
    }
    return best;
}
