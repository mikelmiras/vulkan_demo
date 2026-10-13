#include "physics/soft_body.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace softbody {

bool build(const SoftBody& s, const MeshLibrary& meshes, int mesh, Build& out) {
    out = {};
    if (s.shape == SoftBody::Shape::Cloth) {
        int n = std::clamp(s.resolution, 2, 64);
        float w = std::max(s.width, 0.01f), h = std::max(s.height, 0.01f);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {   // (row 0 is the top)
                out.particles.push_back({-w * 0.5f + w * (float)x / (float)(n - 1), h * 0.5f - h * (float)y / (float)(n - 1), 0.f});
                bool pin = (s.pinned == "top" && y == 0) || (s.pinned == "corners" && y == 0 && (x == 0 || x == n - 1));
                out.invMass.push_back(pin ? 0.f : 1.f);
            }
        for (int y = 0; y + 1 < n; ++y)
            for (int x = 0; x + 1 < n; ++x) {
                uint32_t a = (uint32_t)(y * n + x), b = a + 1, c = a + (uint32_t)n, d = c + 1;
                out.faces.insert(out.faces.end(), {a, c, b, b, c, d});   // (facing +Z)
            }
        // Drawn: both sides (the back's triangles turned round, its normals reversed). A cloth that tears shares no
        // drawn vertices between triangles, so a torn one can be folded away into a point (the mesh keeps its size)
        size_t count = out.particles.size();
        bool tears = s.tearStretch > 0.f;
        auto vertex = [&](uint32_t i, int side, int face) {
            Vertex v;
            v.pos = out.particles[i];
            v.normal = {0, 0, side ? -1.f : 1.f};
            v.u = (float)(i % (uint32_t)n) / (float)(n - 1);
            v.v = (float)(i / (uint32_t)n) / (float)(n - 1);
            out.indices.push_back((uint32_t)out.vertices.size());
            out.vertices.push_back(v);
            out.particleOf.push_back(i);
            out.side.push_back(side ? -1.f : 1.f);
            out.faceOf.push_back(face);
        };
        if (tears) {
            for (int side = 0; side < 2; ++side)
                for (size_t f = 0; f + 2 < out.faces.size(); f += 3) {
                    const uint32_t* t = &out.faces[f];
                    if (side == 0) for (int k : {0, 1, 2}) vertex(t[k], 0, (int)(f / 3));
                    else for (int k : {0, 2, 1}) vertex(t[k], 1, (int)(f / 3));
                }
        } else {
            for (int side = 0; side < 2; ++side)
                for (size_t i = 0; i < count; ++i) vertex((uint32_t)i, side, -1);
            out.indices = out.faces;
            for (size_t f = 0; f + 2 < out.faces.size(); f += 3)
                out.indices.insert(out.indices.end(), {out.faces[f] + (uint32_t)count, out.faces[f + 2] + (uint32_t)count,
                                                       out.faces[f + 1] + (uint32_t)count});
        }
    } else {
        if (mesh < 0 || mesh >= meshes.count()) return false;
        const Mesh& m = meshes.get(mesh);
        // The mesh's vertices (as drawn), welded by position into particles
        std::map<std::tuple<long, long, long>, uint32_t> weld;
        std::map<uint32_t, uint32_t> local;   // library vertex -> drawn vertex
        for (uint32_t k = 0; k < m.indexCount; ++k) {
            uint32_t global = (uint32_t)m.vertexOffset + meshes.indices[m.firstIndex + k];
            auto [it, added] = local.try_emplace(global, (uint32_t)out.vertices.size());
            if (added) {
                const Vertex& v = meshes.vertices[global];
                auto key = std::make_tuple(std::lround(v.pos.x * 1e4f), std::lround(v.pos.y * 1e4f), std::lround(v.pos.z * 1e4f));
                auto [w, fresh] = weld.try_emplace(key, (uint32_t)out.particles.size());
                if (fresh) {
                    out.particles.push_back(v.pos);
                    out.invMass.push_back(1.f);
                }
                out.vertices.push_back(v);
                out.particleOf.push_back(w->second);
                out.side.push_back(1.f);
                out.faceOf.push_back(-1);
            }
            out.indices.push_back(it->second);
        }
        for (size_t f = 0; f + 2 < out.indices.size(); f += 3) {
            uint32_t a = out.particleOf[out.indices[f]], b = out.particleOf[out.indices[f + 1]], c = out.particleOf[out.indices[f + 2]];
            if (a != b && b != c && a != c) out.faces.insert(out.faces.end(), {a, b, c});
        }
    }
    // Mass shared out among the particles that aren't pinned
    size_t free = (size_t)std::count_if(out.invMass.begin(), out.invMass.end(), [](float m) { return m > 0.f; });
    float each = std::max(s.mass, 1e-3f) / (float)std::max<size_t>(free, 1);
    for (float& m : out.invMass)
        if (m > 0.f) m = 1.f / each;
    return out.particles.size() >= 3 && !out.faces.empty();
}

std::vector<uint32_t> tearing(const std::vector<Vec3>& rest, const std::vector<Vec3>& now, const std::vector<uint32_t>& faces,
                              const std::vector<uint32_t>& torn, float tearStretch) {
    std::vector<uint32_t> out;
    if (tearStretch <= 0.f || rest.size() != now.size()) return out;
    float limit = std::max(tearStretch, 1.f);
    for (size_t f = 0; f + 2 < faces.size(); f += 3) {
        uint32_t face = (uint32_t)(f / 3);
        if (std::binary_search(torn.begin(), torn.end(), face)) continue;
        for (int k = 0; k < 3; ++k) {
            uint32_t a = faces[f + (size_t)k], b = faces[f + (size_t)(k + 1) % 3];
            if (a >= rest.size() || b >= rest.size()) continue;
            if (length(now[a] - now[b]) > length(rest[a] - rest[b]) * limit) {
                out.push_back(face);
                break;
            }
        }
    }
    return out;
}

std::vector<Vec3> particleNormals(const std::vector<Vec3>& particles, const std::vector<uint32_t>& faces) {
    std::vector<Vec3> n(particles.size());
    for (size_t f = 0; f + 2 < faces.size(); f += 3) {
        uint32_t a = faces[f], b = faces[f + 1], c = faces[f + 2];
        if (a >= n.size() || b >= n.size() || c >= n.size()) continue;
        Vec3 fn = cross(particles[b] - particles[a], particles[c] - particles[a]);   // (area weighted)
        n[a] = n[a] + fn;
        n[b] = n[b] + fn;
        n[c] = n[c] + fn;
    }
    for (Vec3& v : n) v = length(v) > 1e-12f ? normalize(v) : Vec3{0, 1, 0};
    return n;
}

void syncMeshes(Scene& scene, MeshLibrary& meshes) {
    for (Entity& e : scene.entities) {
        if (!e.softBody) continue;
        SoftBody& s = *e.softBody;
        // (it draws its own deforming copy: a mesh one is made from the MeshRenderer's mesh, which stays as it is)
        int source = e.meshRenderer ? e.meshRenderer->mesh : -1;
        Build b;
        if (!build(s, meshes, source, b)) continue;
        bool fits = s.mesh >= (int)MeshKind::Count && s.mesh < meshes.count() && s.meshVertices == (int)b.vertices.size() &&
                    s.meshSource == source;
        if (fits && s.meshVersion == s.positionsVersion) continue;   // (nothing moved since)
        // Where the particles are now (or at rest), and their normals
        std::vector<Vec3> at = s.positions.size() == b.particles.size() ? s.positions : b.particles;
        // (torn triangles: no part of the normals, folded into their first corner)
        auto torn = [&](uint32_t face) { return std::binary_search(s.torn.begin(), s.torn.end(), face); };
        std::vector<uint32_t> kept;
        for (size_t f = 0; f + 2 < b.faces.size(); f += 3)
            if (!torn((uint32_t)(f / 3))) kept.insert(kept.end(), b.faces.begin() + (long)f, b.faces.begin() + (long)f + 3);
        std::vector<Vec3> normals = particleNormals(at, kept);
        for (size_t i = 0; i < b.vertices.size(); ++i) {
            uint32_t p = b.particleOf[i];
            if (b.faceOf[i] >= 0 && torn((uint32_t)b.faceOf[i])) p = b.particleOf[i - i % 3];
            b.vertices[i].pos = at[p];
            b.vertices[i].normal = normals[p] * b.side[i];
        }
        Vec3 lo = b.vertices[0].pos, hi = lo;
        for (const Vertex& v : b.vertices) {
            lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
            hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
        }
        if (!fits) {
            s.mesh = addMesh(meshes, "Soft Body", "", b.vertices, b.indices);
            s.meshVertices = (int)b.vertices.size();
            s.meshSource = source;
        } else {
            Mesh& m = meshes.meshes[(size_t)s.mesh];
            std::copy(b.vertices.begin(), b.vertices.end(), meshes.vertices.begin() + m.vertexOffset);
            meshes.rewritten[(uint32_t)m.vertexOffset] = {(uint32_t)b.vertices.size(), ++meshes.rewriteSerial};
            ++meshes.version;
        }
        Mesh& m = meshes.meshes[(size_t)s.mesh];
        m.boundsMin = lo;
        m.boundsMax = hi;
        s.meshVersion = s.positionsVersion;
    }
}

}  // namespace softbody
