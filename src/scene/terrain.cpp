#include "scene/terrain.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <set>

#include "core/compress.h"
#include "core/vfs.h"
#include "stb_image.h"

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
    t.holes.clear();
    t.foliageDensity.clear();
    touch(t);
}

void resize(Terrain& t, int resolution) {
    if (resolution == t.resolution || resolution < 2) return;
    Terrain old = t;
    t.resolution = resolution;
    size_t n = (size_t)resolution * (size_t)resolution;
    t.heights.assign(n, 0.f);
    t.weights.assign(n * 4, 0);
    t.holes.clear();
    t.foliageDensity.clear();
    float scale = (float)(old.resolution - 1) / (float)(resolution - 1);
    for (int z = 0; z < resolution; ++z)
        for (int x = 0; x < resolution; ++x) {
            Vec3 p = pointPosition(t, x, z);
            size_t i = (size_t)z * (size_t)resolution + (size_t)x;
            t.heights[i] = heightAt(old, p.x, p.z);
            int ox = clampi((int)std::lround((float)x * scale), 0, old.resolution - 1);
            int oz = clampi((int)std::lround((float)z * scale), 0, old.resolution - 1);
            size_t o = (size_t)oz * (size_t)old.resolution + (size_t)ox;
            std::memcpy(&t.weights[i * 4], &old.weights[o * 4], 4);
            if (!old.holes.empty()) {
                t.holes.resize(n, 0);
                t.holes[i] = old.holes[o];
            }
            if (old.foliageDensity.size() == (size_t)old.resolution * old.resolution * Terrain::kFoliageLayers) {
                t.foliageDensity.resize(n * Terrain::kFoliageLayers, 0);
                std::memcpy(&t.foliageDensity[i * Terrain::kFoliageLayers], &old.foliageDensity[o * Terrain::kFoliageLayers],
                            Terrain::kFoliageLayers);
            }
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

bool holeAt(const Terrain& t, float x, float z) {
    if (t.holes.empty() || t.resolution < 2) return false;
    float gx, gz;
    toGrid(t, x, z, gx, gz);
    float maxIdx = (float)(t.resolution - 1);
    if (gx < 0.f || gz < 0.f || gx > maxIdx || gz > maxIdx) return false;
    int x0 = std::min((int)gx, t.resolution - 2), z0 = std::min((int)gz, t.resolution - 2);
    float fx = gx - (float)x0, fz = gz - (float)z0;
    auto hole = [&](int x, int z) { return t.holes[(size_t)z * (size_t)t.resolution + (size_t)x] != 0; };
    // The triangle under the point (the diagonal from (x0, z0) to (x0 + 1, z0 + 1)): any hole corner and it's gone
    if (hole(x0, z0) || hole(x0 + 1, z0 + 1)) return true;
    return fz >= fx ? hole(x0, z0 + 1) : hole(x0 + 1, z0);
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
    auto above = [&](float s) {   // (through holes: never below)
        Vec3 p = origin + dir * s;
        if (holeAt(t, p.x, p.z)) return std::max(p.y - heightAt(t, p.x, p.z), 1e-3f);
        return p.y - heightAt(t, p.x, p.z);
    };
    float step = cellSize(t) * 0.5f, prev = t0, prevH = above(t0);
    if (prevH <= 0.f && !holeAt(t, (origin + dir * t0).x, (origin + dir * t0).z)) {   // starts under the surface
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
            if (b.mode == Brush::Holes || b.mode == Brush::Fill) {   // (a point is in or out: the inner half of the brush)
                if (f < 0.5f) continue;
                if (b.mode == Brush::Holes && t.holes.empty()) t.holes.assign((size_t)t.resolution * t.resolution, 0);
                if (t.holes.empty() || t.holes[i] == (b.mode == Brush::Holes ? 1 : 0)) continue;
                t.holes[i] = b.mode == Brush::Holes ? 1 : 0;
                changed = true;
                continue;
            }
            if (b.mode == Brush::Foliage) {
                size_t layers = Terrain::kFoliageLayers;
                if (t.foliageDensity.size() != (size_t)t.resolution * t.resolution * layers)
                    t.foliageDensity.assign((size_t)t.resolution * t.resolution * layers, 0);
                uint8_t& d = t.foliageDensity[i * layers + (size_t)clampi(b.layer, 0, (int)layers - 1)];
                int step = std::max(1, (int)std::lround(255.f * 3.f * amount * f));
                int next = clampi(d + (b.erase ? -step : step), 0, 255);
                if (next == d) continue;
                d = (uint8_t)next;
                changed = true;
                continue;
            }
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
                default: break;
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
    add(t.holes.data(), t.holes.size());
    return h;
}

namespace {

// One chunk's triangles at a level (every `step`th point; the chunk's last row and column always), and the skirts
// hanging from its edges (skirt[i]: the dropped copy of edge point i)
void chunkIndices(const Terrain& t, int cx, int cz, int step, const std::vector<int32_t>& skirt, std::vector<uint32_t>& out) {
    int cells = t.resolution - 1;
    int x0 = cx * kChunkCells, x1 = std::min(x0 + kChunkCells, cells), z0 = cz * kChunkCells, z1 = std::min(z0 + kChunkCells, cells);
    auto line = [&](int a, int b) {
        std::vector<int> v;
        for (int k = a; k < b; k += step) v.push_back(k);
        v.push_back(b);
        return v;
    };
    std::vector<int> xs = line(x0, x1), zs = line(z0, z1);
    auto at = [&](int x, int z) { return (uint32_t)(z * t.resolution + x); };
    for (size_t j = 0; j + 1 < zs.size(); ++j)
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            uint32_t a = at(xs[i], zs[j]), b = at(xs[i + 1], zs[j]), c = at(xs[i], zs[j + 1]), d = at(xs[i + 1], zs[j + 1]);
            out.insert(out.end(), {a, c, d, a, d, b});   // (the diagonal a-d, like heightAt and Jolt)
        }
    auto skirtAlong = [&](const std::vector<int>& run, bool alongX, int fixed) {
        for (size_t k = 0; k + 1 < run.size(); ++k) {
            uint32_t p = alongX ? at(run[k], fixed) : at(fixed, run[k]), q = alongX ? at(run[k + 1], fixed) : at(fixed, run[k + 1]);
            uint32_t sp = (uint32_t)skirt[p], sq = (uint32_t)skirt[q];
            out.insert(out.end(), {p, q, sq, p, sq, sp, p, sq, q, p, sp, sq});   // (both sides)
        }
    };
    skirtAlong(xs, true, z0);
    skirtAlong(xs, true, z1);
    skirtAlong(zs, false, x0);
    skirtAlong(zs, false, x1);
}

}  // namespace

int grassBladeMesh(MeshLibrary& meshes) {
    int found = meshes.find("Grass Blade");
    if (found >= 0) return found;
    // Three tapering blades turned 60 degrees apart, leaning a little; normals mostly up (lit like the ground)
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    for (int b = 0; b < 3; ++b) {
        float a = (float)b * 2.094f + 0.3f, ca = std::cos(a), sa = std::sin(a);
        Vec3 side{ca, 0.f, sa}, lean{-sa * 0.15f, 0.f, ca * 0.15f};
        Vec3 offset{side.z * 0.04f, 0.f, -side.x * 0.04f};
        uint32_t base = (uint32_t)verts.size();
        const int segments = 3;
        for (int k = 0; k <= segments; ++k) {
            float h = (float)k / (float)segments, w = 0.035f * (1.f - h);
            Vec3 centre = offset + Vec3{0.f, 0.5f * h, 0.f} + lean * (h * h);
            Vec3 n = normalize(Vec3{0, 1, 0} + cross(side, Vec3{0, 1, 0}) * 0.4f);
            verts.push_back({centre - side * w, n, 0.f, 1.f - h});
            if (k < segments) verts.push_back({centre + side * w, n, 1.f, 1.f - h});
        }
        for (int k = 0; k < segments; ++k) {
            uint32_t l0 = base + (uint32_t)k * 2, r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
            if (k + 1 == segments) idx.insert(idx.end(), {l0, r0, l1});   // (the tip)
            else idx.insert(idx.end(), {l0, r0, l1, l1, r0, r1});
        }
    }
    return addMesh(meshes, "Grass Blade", "", verts, idx);
}

void scatterFoliage(Terrain& t) {
    t.plants.clear();
    t.plantRanges.clear();
    t.plantRevision = t.revision;
    const size_t layers = Terrain::kFoliageLayers;
    size_t n = (size_t)t.resolution * (size_t)t.resolution;
    if (t.foliage.empty() || t.resolution < 2 || t.foliageDensity.size() != n * layers) return;
    const size_t kMaxPlants = 300000;
    float cell = cellSize(t);
    int cells = t.resolution - 1, chunks = (cells + kChunkCells - 1) / kChunkCells;
    auto density = [&](int x, int z, size_t layer) {
        return t.foliageDensity[((size_t)z * (size_t)t.resolution + (size_t)x) * layers + layer] / 255.f;
    };
    for (int cz = 0; cz < chunks; ++cz)
        for (int cx = 0; cx < chunks; ++cx)
            for (size_t L = 0; L < t.foliage.size() && L < layers; ++L) {
                const Terrain::Foliage& f = t.foliage[L];
                Terrain::PlantRange range;
                range.layer = (int)L;
                range.first = (uint32_t)t.plants.size();
                range.lo = {1e30f, 1e30f, 1e30f};
                range.hi = {-1e30f, -1e30f, -1e30f};
                for (int z = cz * kChunkCells; z < std::min((cz + 1) * kChunkCells, cells); ++z)
                    for (int x = cx * kChunkCells; x < std::min((cx + 1) * kChunkCells, cells); ++x) {
                        float paint = 0.25f * (density(x, z, L) + density(x + 1, z, L) + density(x, z + 1, L) + density(x + 1, z + 1, L));
                        if (paint <= 0.f) continue;
                        float expected = std::min(std::max(f.density, 0.f) * cell * cell * paint, 64.f);
                        // (the same scattering every time: random numbers from the cell and layer)
                        uint32_t rng = (uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u ^ (uint32_t)(L + 1) * 83492791u;
                        auto rand01 = [&] {
                            rng ^= rng << 13;
                            rng ^= rng >> 17;
                            rng ^= rng << 5;
                            return (float)(rng & 0xFFFFFF) / (float)0x1000000;
                        };
                        rand01();
                        int count = (int)expected + (rand01() < expected - std::floor(expected) ? 1 : 0);
                        for (int k = 0; k < count && t.plants.size() < kMaxPlants; ++k) {
                            float px = -t.size * 0.5f + ((float)x + rand01()) * cell, pz = -t.size * 0.5f + ((float)z + rand01()) * cell;
                            float yaw = rand01() * 6.2831853f, scale = f.minScale + (f.maxScale - f.minScale) * rand01();
                            float shade = 0.8f + 0.4f * rand01();
                            if (holeAt(t, px, pz)) continue;
                            float py = heightAt(t, px, pz);
                            t.plants.push_back({{px, py, pz}, yaw, scale, shade});
                            range.lo = {std::min(range.lo.x, px), std::min(range.lo.y, py), std::min(range.lo.z, pz)};
                            range.hi = {std::max(range.hi.x, px), std::max(range.hi.y, py), std::max(range.hi.z, pz)};
                        }
                    }
                range.count = (uint32_t)t.plants.size() - range.first;
                if (range.count) t.plantRanges.push_back(range);
            }
}

void syncMeshes(Scene& scene, MeshLibrary& meshes) {
    std::set<int> claimed;   // (a duplicated terrain starts out with its original's meshes: it needs its own)
    for (Entity& e : scene.entities) {
        if (!e.terrain) continue;
        Terrain& t = *e.terrain;
        size_t n = (size_t)t.resolution * (size_t)t.resolution;
        if (t.resolution < 2 || t.heights.size() != n || t.weights.size() != n * 4) continue;
        if (t.revision == 0) touch(t);
        for (const Terrain::Foliage& f : t.foliage)
            if (f.mesh == "Grass Blade") grassBladeMesh(meshes);
        if (t.plantRevision != t.revision) scatterFoliage(t);
        const int cells = t.resolution - 1, chunks = (cells + kChunkCells - 1) / kChunkCells;
        bool own = t.meshResolution == t.resolution && t.meshes.size() == (size_t)(chunks * chunks);
        for (int id : t.meshes)
            own = own && id >= (int)MeshKind::Count && id < meshes.count() && !claimed.count(id) && meshes.get(id).source.empty();
        if (!own) t.meshes.clear();
        for (int id : t.meshes) claimed.insert(id);
        if (!t.meshes.empty() && t.meshRevision == t.revision) continue;

        // Skirts: a dropped copy of every point on a chunk edge
        std::vector<int32_t> skirt(n, -1);
        int32_t extra = 0;
        for (int z = 0; z < t.resolution; ++z)
            for (int x = 0; x < t.resolution; ++x)
                if (x % kChunkCells == 0 || z % kChunkCells == 0 || x == cells || z == cells)
                    skirt[(size_t)z * (size_t)t.resolution + (size_t)x] = (int32_t)n + extra++;
        // The vertices: the layer weights ride in the skin weights, the hole flag in the first joint (terrains
        // aren't skinned)
        std::vector<Vertex> verts(n + (size_t)extra);
        std::vector<SkinVertex> skin(n + (size_t)extra);
        float cell = cellSize(t), drop = cell * 6.f;
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
                skin[i].joints[0] = !t.holes.empty() && t.holes[i] ? 1 : 0;
                if (skirt[i] >= 0) {
                    verts[(size_t)skirt[i]] = v;
                    verts[(size_t)skirt[i]].pos.y -= drop;
                    skin[(size_t)skirt[i]] = skin[i];
                }
            }
        if (t.meshes.empty()) {
            // A mesh per chunk, all over the same vertices; its two coarser levels' indices after its own
            t.meshes.clear();
            t.chunkLods.clear();
            int32_t base = (int32_t)meshes.vertices.size();
            meshes.skin.resize(meshes.vertices.size());   // (built-in meshes add vertices directly)
            meshes.vertices.insert(meshes.vertices.end(), verts.begin(), verts.end());
            meshes.skin.insert(meshes.skin.end(), skin.begin(), skin.end());
            for (int cz = 0; cz < chunks; ++cz)
                for (int cx = 0; cx < chunks; ++cx) {
                    std::vector<uint32_t> idx;
                    chunkIndices(t, cx, cz, 1, skirt, idx);
                    Mesh m;
                    m.name = "Terrain";
                    m.vertexOffset = base;
                    m.firstIndex = (uint32_t)meshes.indices.size();
                    m.indexCount = (uint32_t)idx.size();
                    meshes.indices.insert(meshes.indices.end(), idx.begin(), idx.end());
                    for (int step : {2, 4}) {
                        std::vector<uint32_t> coarse;
                        chunkIndices(t, cx, cz, step, skirt, coarse);
                        t.chunkLods.push_back((uint32_t)meshes.indices.size());
                        t.chunkLods.push_back((uint32_t)coarse.size());
                        meshes.indices.insert(meshes.indices.end(), coarse.begin(), coarse.end());
                    }
                    meshes.meshes.push_back(m);
                    t.meshes.push_back(meshes.count() - 1);
                    claimed.insert(meshes.count() - 1);
                }
            t.meshResolution = t.resolution;
        } else {   // same size: rewritten in place
            int base = meshes.get(t.meshes[0]).vertexOffset;
            std::copy(verts.begin(), verts.end(), meshes.vertices.begin() + base);
            std::copy(skin.begin(), skin.end(), meshes.skin.begin() + base);
            meshes.rewritten[(uint32_t)base] = {(uint32_t)verts.size(), ++meshes.rewriteSerial};
        }
        ++meshes.version;
        // Each chunk's bounds, and its levels of detail (none while it has holes: they'd fill them at a distance)
        for (int cz = 0; cz < chunks; ++cz)
            for (int cx = 0; cx < chunks; ++cx) {
                size_t k = (size_t)(cz * chunks + cx);
                Mesh& m = meshes.meshes[(size_t)t.meshes[k]];
                Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
                bool holes = false;
                for (int z = cz * kChunkCells; z <= std::min((cz + 1) * kChunkCells, cells); ++z)
                    for (int x = cx * kChunkCells; x <= std::min((cx + 1) * kChunkCells, cells); ++x) {
                        size_t i = (size_t)z * (size_t)t.resolution + (size_t)x;
                        Vec3 p = verts[i].pos;
                        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y - drop), std::min(lo.z, p.z)};
                        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                        holes = holes || (!t.holes.empty() && t.holes[i]);
                    }
                m.boundsMin = lo;
                m.boundsMax = hi;
                m.lods.clear();
                if (!holes)
                    for (int l = 0; l < 2; ++l)
                        m.lods.push_back({t.chunkLods[k * 4 + (size_t)l * 2], t.chunkLods[k * 4 + (size_t)l * 2 + 1], m.vertexOffset});
            }
        t.meshRevision = t.revision;
    }
}

bool importHeightmap(Terrain& t, const std::string& path, float height, std::string& error) {
    if (t.resolution < 2) {
        error = "the terrain isn't made yet";
        return false;
    }
    std::string bytes;
    if (!vfs::read(std::filesystem::u8path(path), bytes)) {
        error = "cannot open " + path;
        return false;
    }
    int w = 0, h = 0, channels = 0;
    stbi_us* pixels = stbi_load_16_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &channels, 1);
    if (!pixels) {
        error = path + ": not an image this can read";
        return false;
    }
    for (int z = 0; z < t.resolution; ++z)
        for (int x = 0; x < t.resolution; ++x) {   // (bilinear: the image's corners on the terrain's corners)
            float fx = (float)x / (float)(t.resolution - 1) * (float)(w - 1), fz = (float)z / (float)(t.resolution - 1) * (float)(h - 1);
            int x0 = std::min((int)fx, w - 1), z0 = std::min((int)fz, h - 1), x1 = std::min(x0 + 1, w - 1), z1 = std::min(z0 + 1, h - 1);
            float tx = fx - (float)x0, tz = fz - (float)z0;
            auto at = [&](int px, int pz) { return (float)pixels[(size_t)pz * (size_t)w + (size_t)px] / 65535.f; };
            float top = at(x0, z0) + (at(x1, z0) - at(x0, z0)) * tx, bottom = at(x0, z1) + (at(x1, z1) - at(x0, z1)) * tx;
            t.heights[(size_t)z * (size_t)t.resolution + (size_t)x] = (top + (bottom - top) * tz) * height;
        }
    stbi_image_free(pixels);
    touch(t);
    return true;
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

std::string encodeBytes(const std::vector<uint8_t>& bytes) { return pack(std::string(bytes.begin(), bytes.end())); }

bool decodeBytes(const std::string& text, size_t expected, std::vector<uint8_t>& out) {
    std::string raw;
    if (!unpack(text, raw, expected)) return false;
    out.assign(raw.begin(), raw.end());
    return true;
}

bool decodeWeights(Terrain& t, const std::string& text) {
    size_t n = (size_t)t.resolution * (size_t)t.resolution * 4;
    std::string raw;
    if (!unpack(text, raw, n)) return false;
    t.weights.assign(raw.begin(), raw.end());
    return true;
}

}  // namespace terrain
