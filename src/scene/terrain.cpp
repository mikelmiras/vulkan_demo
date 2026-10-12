#include "scene/terrain.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <set>

#include "core/compress.h"

namespace terrain {

namespace {

std::atomic<uint64_t> g_revision{0};

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

float sampleAt(const Terrain& t, int x, int z) {
    int n = t.resolution;
    return t.heights[(size_t)clampi(z, 0, n - 1) * (size_t)n + (size_t)clampi(x, 0, n - 1)];
}

// Grid coordinates (in samples) of a local position
void toGrid(const Terrain& t, float x, float z, float& gx, float& gz) {
    float c = cellSize(t);
    gx = (x + t.size * 0.5f) / c;
    gz = (z + t.size * 0.5f) / c;
}

// Smooth brush falloff: 1 at the centre, 0 at the rim
float falloff(float d, float r) {
    if (d >= r) return 0.f;
    float u = d / r;
    return 1.f - u * u * (3.f - 2.f * u);
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint32_t)(uint8_t)in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (uint32_t)(uint8_t)in[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[v & 63] : '=';
    }
    return out;
}

bool unbase64(const std::string& in, std::string& out) {
    int map[256];
    std::fill(std::begin(map), std::end(map), -1);
    for (int i = 0; i < 64; ++i) map[(uint8_t)kB64[i]] = i;
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int d = map[(uint8_t)c];
        if (d < 0) return false;
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((v >> bits) & 0xFF);
        }
    }
    return true;
}

// Stored: [raw size (u32)] then the bytes compressed
std::string pack(const std::string& raw) {
    uint32_t n = (uint32_t)raw.size();
    std::string out(reinterpret_cast<const char*>(&n), 4);
    out += lz::compress(raw.data(), raw.size());
    return base64(out);
}

bool unpack(const std::string& text, std::string& raw, size_t expected) {
    std::string bytes;
    if (!unbase64(text, bytes) || bytes.size() < 4) return false;
    uint32_t n;
    std::memcpy(&n, bytes.data(), 4);
    if (n != expected) return false;
    return lz::decompress(bytes.data() + 4, bytes.size() - 4, raw, n);
}

// Value noise: smooth random values on a lattice
float latticeValue(int x, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFF) / (float)0xFFFFFF;
}
float valueNoise(float x, float z, uint32_t seed) {
    int x0 = (int)std::floor(x), z0 = (int)std::floor(z);
    float fx = x - (float)x0, fz = z - (float)z0;
    fx = fx * fx * (3.f - 2.f * fx);
    fz = fz * fz * (3.f - 2.f * fz);
    float a = latticeValue(x0, z0, seed), b = latticeValue(x0 + 1, z0, seed);
    float c = latticeValue(x0, z0 + 1, seed), d = latticeValue(x0 + 1, z0 + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}

}  // namespace

float cellSize(const Terrain& t) { return t.resolution > 1 ? t.size / (float)(t.resolution - 1) : 1.f; }

Vec3 pointPosition(const Terrain& t, int x, int z) {
    float c = cellSize(t);
    return {-t.size * 0.5f + (float)x * c, sampleAt(t, x, z), -t.size * 0.5f + (float)z * c};
}

void touch(Terrain& t) { t.revision = ++g_revision; }

void init(Terrain& t, int resolution, float size) {
    t.resolution = std::max(resolution, 2);
    t.size = std::max(size, 1.f);
    size_t n = (size_t)t.resolution * (size_t)t.resolution;
    t.heights.assign(n, 0.f);
    t.weights.assign(n * 4, 0);
    for (size_t i = 0; i < n; ++i) t.weights[i * 4] = 255;
    touch(t);
}

void resize(Terrain& t, int resolution) {
    if (resolution == t.resolution || resolution < 2) return;
    Terrain old = t;
    t.resolution = resolution;
    size_t n = (size_t)resolution * (size_t)resolution;
    t.heights.assign(n, 0.f);
    t.weights.assign(n * 4, 0);
    float scale = (float)(old.resolution - 1) / (float)(resolution - 1);
    for (int z = 0; z < resolution; ++z)
        for (int x = 0; x < resolution; ++x) {
            Vec3 p = pointPosition(t, x, z);
            size_t i = (size_t)z * (size_t)resolution + (size_t)x;
            t.heights[i] = heightAt(old, p.x, p.z);
            int ox = clampi((int)std::lround((float)x * scale), 0, old.resolution - 1);
            int oz = clampi((int)std::lround((float)z * scale), 0, old.resolution - 1);
            std::memcpy(&t.weights[i * 4], &old.weights[((size_t)oz * (size_t)old.resolution + (size_t)ox) * 4], 4);
        }
    touch(t);
}

float heightAt(const Terrain& t, float x, float z) {
    if (t.resolution < 2 || t.heights.empty()) return 0.f;
    float gx, gz;
    toGrid(t, x, z, gx, gz);
    float maxIdx = (float)(t.resolution - 1);
    gx = std::clamp(gx, 0.f, maxIdx);
    gz = std::clamp(gz, 0.f, maxIdx);
    int x0 = std::min((int)gx, t.resolution - 2), z0 = std::min((int)gz, t.resolution - 2);
    float fx = gx - (float)x0, fz = gz - (float)z0;
    float a = sampleAt(t, x0, z0), b = sampleAt(t, x0 + 1, z0), c = sampleAt(t, x0, z0 + 1), d = sampleAt(t, x0 + 1, z0 + 1);
    // The cell's two triangles share the diagonal from (x0, z0) to (x0 + 1, z0 + 1), as the mesh and the collision do
    if (fz >= fx) return a + (d - c) * fx + (c - a) * fz;
    return a + (b - a) * fx + (d - b) * fz;
}

Vec3 normalAt(const Terrain& t, float x, float z) {
    float e = cellSize(t) * 0.5f;
    float dx = heightAt(t, x + e, z) - heightAt(t, x - e, z), dz = heightAt(t, x, z + e) - heightAt(t, x, z - e);
    return normalize(Vec3{-dx, 2.f * e, -dz});
}

bool raycast(const Terrain& t, Vec3 origin, Vec3 dir, float maxDistance, float& hit) {
    if (t.resolution < 2 || t.heights.empty()) return false;
    dir = normalize(dir);
    auto [lo, hi] = std::minmax_element(t.heights.begin(), t.heights.end());
    float half = t.size * 0.5f;
    Vec3 bmin{-half, *lo - 0.01f, -half}, bmax{half, *hi + 0.01f, half};
    // Clip the ray to the terrain's box
    float t0 = 0.f, t1 = maxDistance;
    for (int a = 0; a < 3; ++a) {
        float o = a == 0 ? origin.x : a == 1 ? origin.y : origin.z, d = a == 0 ? dir.x : a == 1 ? dir.y : dir.z;
        float mn = a == 0 ? bmin.x : a == 1 ? bmin.y : bmin.z, mx = a == 0 ? bmax.x : a == 1 ? bmax.y : bmax.z;
        if (std::fabs(d) < 1e-9f) {
            if (o < mn || o > mx) return false;
            continue;
        }
        float ta = (mn - o) / d, tb = (mx - o) / d;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    // March in half cells; then bisect the step that crossed the surface
    auto above = [&](float s) {
        Vec3 p = origin + dir * s;
        return p.y - heightAt(t, p.x, p.z);
    };
    float step = cellSize(t) * 0.5f, prev = t0, prevH = above(t0);
    if (prevH <= 0.f) {   // starts under the surface
        hit = t0;
        return true;
    }
    for (float s = t0 + step;; s += step) {
        s = std::min(s, t1);
        float h = above(s);
        if (h <= 0.f) {
            float a = prev, b = s;
            for (int i = 0; i < 20; ++i) {
                float m = (a + b) * 0.5f;
                if (above(m) > 0.f) a = m;
                else b = m;
            }
            hit = b;
            return true;
        }
        prev = s;
        prevH = h;
        if (s >= t1) return false;
    }
}

bool applyBrush(Terrain& t, const BrushSettings& b, Vec3 c, float dt) {
    if (t.resolution < 2 || b.radius <= 0.f || dt <= 0.f) return false;
    float cell = cellSize(t), half = t.size * 0.5f;
    int x0 = clampi((int)std::floor((c.x - b.radius + half) / cell), 0, t.resolution - 1);
    int x1 = clampi((int)std::ceil((c.x + b.radius + half) / cell), 0, t.resolution - 1);
    int z0 = clampi((int)std::floor((c.z - b.radius + half) / cell), 0, t.resolution - 1);
    int z1 = clampi((int)std::ceil((c.z + b.radius + half) / cell), 0, t.resolution - 1);
    if (x0 > x1 || z0 > z1) return false;
    std::vector<float> before;
    if (b.mode == Brush::Smooth) before = t.heights;
    float amount = std::clamp(b.strength, 0.f, 10.f) * dt;
    bool changed = false;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            Vec3 p = pointPosition(t, x, z);
            float f = falloff(std::sqrt((p.x - c.x) * (p.x - c.x) + (p.z - c.z) * (p.z - c.z)), b.radius);
            if (f <= 0.f) continue;
            size_t i = (size_t)z * (size_t)t.resolution + (size_t)x;
            float& h = t.heights[i];
            switch (b.mode) {
                case Brush::Raise: h += 4.f * amount * f; break;
                case Brush::Lower: h -= 4.f * amount * f; break;
                case Brush::Flatten: h += (b.flattenHeight - h) * std::min(1.f, 4.f * amount * f); break;
                case Brush::Smooth: {
                    float sum = 0.f;
                    int n = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int sx = clampi(x + dx, 0, t.resolution - 1), sz = clampi(z + dz, 0, t.resolution - 1);
                            sum += before[(size_t)sz * (size_t)t.resolution + (size_t)sx];
                            ++n;
                        }
                    h += (sum / (float)n - h) * std::min(1.f, 6.f * amount * f);
                    break;
                }
                case Brush::Paint: {   // the layer's share grows; the others shrink in proportion
                    uint8_t* w = &t.weights[i * 4];
                    int layer = clampi(b.layer, 0, Terrain::kLayers - 1);
                    float cur = w[layer] / 255.f, next = std::min(1.f, cur + 3.f * amount * f);
                    if (next <= cur) break;
                    float rest = 1.f - cur, keep = rest > 1e-6f ? (1.f - next) / rest : 0.f;
                    float vals[Terrain::kLayers];
                    for (int k = 0; k < Terrain::kLayers; ++k) vals[k] = k == layer ? next : w[k] / 255.f * keep;
                    int total = 0, biggest = 0;
                    for (int k = 0; k < Terrain::kLayers; ++k) {
                        w[k] = (uint8_t)std::lround(std::clamp(vals[k], 0.f, 1.f) * 255.f);
                        total += w[k];
                        if (w[k] > w[biggest]) biggest = k;
                    }
                    w[biggest] = (uint8_t)clampi(w[biggest] + 255 - total, 0, 255);   // (exactly 255 in all)
                    break;
                }
            }
            changed = true;
        }
    if (changed) touch(t);
    return changed;
}

void addNoise(Terrain& t, float amplitude, float scale, uint32_t seed) {
    if (t.resolution < 2) return;
    scale = std::max(scale, 0.01f);
    for (int z = 0; z < t.resolution; ++z)
        for (int x = 0; x < t.resolution; ++x) {
            Vec3 p = pointPosition(t, x, z);
            float n = 0.f, amp = 1.f, freq = 1.f / scale, norm = 0.f;
            for (int o = 0; o < 5; ++o) {
                n += valueNoise(p.x * freq, p.z * freq, seed + (uint32_t)o * 101u) * amp;
                norm += amp;
                amp *= 0.5f;
                freq *= 2.f;
            }
            t.heights[(size_t)z * (size_t)t.resolution + (size_t)x] += amplitude * (n / norm);
        }
    touch(t);
}

uint64_t contentHash(const Terrain& t) {
    uint64_t h = 1469598103934665603ull;
    auto add = [&](const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    };
    add(&t.resolution, sizeof(t.resolution));
    add(&t.size, sizeof(t.size));
    add(t.heights.data(), t.heights.size() * sizeof(float));
    return h;
}

void syncMeshes(Scene& scene, MeshLibrary& meshes) {
    std::set<int> claimed;   // (a duplicated terrain starts out with its original's mesh: it needs its own)
    for (Entity& e : scene.entities) {
        if (!e.terrain) continue;
        Terrain& t = *e.terrain;
        size_t n = (size_t)t.resolution * (size_t)t.resolution;
        if (t.resolution < 2 || t.heights.size() != n || t.weights.size() != n * 4) continue;
        if (t.revision == 0) touch(t);
        size_t cells = (size_t)(t.resolution - 1) * (size_t)(t.resolution - 1);
        bool own = t.mesh >= (int)MeshKind::Count && t.mesh < meshes.count() && !claimed.count(t.mesh) &&
                   meshes.get(t.mesh).indexCount == cells * 6 && meshes.get(t.mesh).source.empty();
        if (!own) t.mesh = -1;
        if (t.mesh >= 0) claimed.insert(t.mesh);
        if (t.mesh >= 0 && t.meshRevision == t.revision) continue;

        // The grid's vertices: the layer weights ride in the skin weights (terrains aren't skinned)
        std::vector<Vertex> verts(n);
        std::vector<SkinVertex> skin(n);
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        float cell = cellSize(t);
        for (int z = 0; z < t.resolution; ++z)
            for (int x = 0; x < t.resolution; ++x) {
                size_t i = (size_t)z * (size_t)t.resolution + (size_t)x;
                Vertex& v = verts[i];
                v.pos = pointPosition(t, x, z);
                float dx = sampleAt(t, x + 1, z) - sampleAt(t, x - 1, z), dz = sampleAt(t, x, z + 1) - sampleAt(t, x, z - 1);
                v.normal = normalize(Vec3{-dx, 2.f * cell, -dz});
                v.u = (float)x * cell;   // (metres: layers tile per metre)
                v.v = (float)z * cell;
                for (int k = 0; k < 4; ++k) skin[i].weights[k] = t.weights[i * 4 + (size_t)k] / 255.f;
                lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
                hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
            }
        if (t.mesh < 0) {
            std::vector<uint32_t> idx;
            idx.reserve(cells * 6);
            for (int z = 0; z + 1 < t.resolution; ++z)
                for (int x = 0; x + 1 < t.resolution; ++x) {
                    uint32_t a = (uint32_t)(z * t.resolution + x), b = a + 1, c = a + (uint32_t)t.resolution, d = c + 1;
                    idx.insert(idx.end(), {a, c, d, a, d, b});   // (the diagonal a-d, like heightAt and Jolt)
                }
            t.mesh = addMesh(meshes, "Terrain", "", verts, idx);
            claimed.insert(t.mesh);
            Mesh& m = meshes.meshes[(size_t)t.mesh];
            for (size_t i = 0; i < n; ++i) meshes.skin[(size_t)m.vertexOffset + i] = skin[i];
        } else {   // same size: rewritten in place
            Mesh& m = meshes.meshes[(size_t)t.mesh];
            std::copy(verts.begin(), verts.end(), meshes.vertices.begin() + m.vertexOffset);
            std::copy(skin.begin(), skin.end(), meshes.skin.begin() + m.vertexOffset);
            meshes.rewritten[(uint32_t)m.vertexOffset] = {(uint32_t)n, ++meshes.rewriteSerial};
            ++meshes.version;
        }
        Mesh& m = meshes.meshes[(size_t)t.mesh];
        m.boundsMin = lo;
        m.boundsMax = hi;
        t.meshRevision = t.revision;
    }
}

std::string encodeHeights(const Terrain& t, float& lowest, float& highest) {
    lowest = highest = 0.f;
    if (t.heights.empty()) return {};
    auto [lo, hi] = std::minmax_element(t.heights.begin(), t.heights.end());
    lowest = *lo;
    highest = *hi;
    float range = highest - lowest;
    std::string raw(t.heights.size() * 2, '\0');
    for (size_t i = 0; i < t.heights.size(); ++i) {
        uint16_t q = range > 0.f ? (uint16_t)std::lround((t.heights[i] - lowest) / range * 65535.f) : 0;
        std::memcpy(&raw[i * 2], &q, 2);
    }
    return pack(raw);
}

bool decodeHeights(Terrain& t, const std::string& text, float lowest, float highest) {
    size_t n = (size_t)t.resolution * (size_t)t.resolution;
    std::string raw;
    if (!unpack(text, raw, n * 2)) return false;
    t.heights.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint16_t q;
        std::memcpy(&q, &raw[i * 2], 2);
        t.heights[i] = lowest + (highest - lowest) * (float)q / 65535.f;
    }
    return true;
}

std::string encodeWeights(const Terrain& t) { return pack(std::string(t.weights.begin(), t.weights.end())); }

bool decodeWeights(Terrain& t, const std::string& text) {
    size_t n = (size_t)t.resolution * (size_t)t.resolution * 4;
    std::string raw;
    if (!unpack(text, raw, n)) return false;
    t.weights.assign(raw.begin(), raw.end());
    return true;
}

}  // namespace terrain
