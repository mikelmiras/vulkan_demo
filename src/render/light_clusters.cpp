#include "render/light_clusters.h"

#include <algorithm>
#include <cmath>
#include <map>

int clusterSlice(const ClusterSetup& s, float depth) {
    if (depth <= s.near) return 0;
    int k = (int)std::floor(std::log(depth / s.near) / std::log(s.far / s.near) * (float)s.slices);
    return std::clamp(k, 0, s.slices - 1);
}

void fitDepthRange(const Mat4& viewProj, Vec3 camPos, Vec3 camForward, const std::vector<std::pair<Vec3, Vec3>>& boxes,
                   float& near, float& far) {
    const float* m = viewProj.m;
    Vec3 fwd = normalize(camForward);
    float lo = 1e30f, hi = -1e30f;
    for (const auto& [a, b] : boxes) {
        // Outside the frustum when every corner is beyond one of its planes (clip space: x, y in [-w, w], z in [0, w])
        int outside[6] = {0, 0, 0, 0, 0, 0};
        float d0 = 1e30f, d1 = -1e30f;
        for (int c = 0; c < 8; ++c) {
            Vec3 p{c & 1 ? b.x : a.x, c & 2 ? b.y : a.y, c & 4 ? b.z : a.z};
            float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
            float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
            float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            outside[0] += x < -w, outside[1] += x > w, outside[2] += y < -w, outside[3] += y > w, outside[4] += z < 0.f,
                outside[5] += z > w;
            float d = dot(p - camPos, fwd);
            d0 = std::min(d0, d), d1 = std::max(d1, d);
        }
        bool visible = true;
        for (int k = 0; k < 6; ++k) visible = visible && outside[k] < 8;
        if (!visible) continue;
        lo = std::min(lo, d0), hi = std::max(hi, d1);
    }
    if (lo > hi) return;
    float n = std::clamp(lo, near, far), f = std::clamp(hi, near, far);
    near = n;
    far = std::max(f, n * 1.5f);
}

int clusterOf(const ClusterSetup& s, Vec3 p) {
    const float* m = s.viewProj.m;
    float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (w <= 1e-6f) return -1;
    float u = x / w * 0.5f + 0.5f, v = y / w * 0.5f + 0.5f;
    if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) return -1;
    int tx = std::min((int)(u * (float)s.tilesX), s.tilesX - 1), ty = std::min((int)(v * (float)s.tilesY), s.tilesY - 1);
    int slice = clusterSlice(s, dot(p - s.camPos, normalize(s.camForward)));
    return (slice * s.tilesY + ty) * s.tilesX + tx;
}

bool buildLightClusters(const ClusterSetup& s, const std::vector<ClusterLight>& lights, std::vector<uint32_t>& out,
                        size_t maxIndices) {
    const int n = s.clusters();
    Vec3 fwd = normalize(s.camForward);
    // Each light's cluster box: tiles [x0, x1] x [y0, y1], slices [z0, z1] (empty if it's out of view)
    struct Box {
        int x0, x1, y0, y1, z0, z1;
    };
    std::vector<Box> boxes(lights.size());
    std::vector<uint32_t> counts((size_t)n, 0);
    const float* m = s.viewProj.m;
    for (size_t i = 0; i < lights.size(); ++i) {
        const ClusterLight& l = lights[i];
        Box& b = boxes[i];
        b = {0, -1, 0, -1, 0, -1};
        float r = std::max(l.range, 0.f);
        float depth = dot(l.position - s.camPos, fwd);
        if (depth + r <= s.near * 0.5f || depth - r >= s.far) continue;   // behind the camera or beyond the grid
        // Screen box: the corners of the sphere's bounding box (all of the screen if one is behind the camera)
        float u0 = 1.f, u1 = 0.f, v0 = 1.f, v1 = 0.f;
        bool whole = false;
        for (int c = 0; c < 8 && !whole; ++c) {
            Vec3 p = l.position + Vec3{c & 1 ? r : -r, c & 2 ? r : -r, c & 4 ? r : -r};
            float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
            float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
            float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
            if (w <= 1e-4f) { whole = true; break; }
            float u = x / w * 0.5f + 0.5f, v = y / w * 0.5f + 0.5f;
            u0 = std::min(u0, u); u1 = std::max(u1, u);
            v0 = std::min(v0, v); v1 = std::max(v1, v);
        }
        if (whole) { u0 = v0 = 0.f; u1 = v1 = 1.f; }
        if (u1 < 0.f || u0 >= 1.f || v1 < 0.f || v0 >= 1.f) continue;   // off to a side
        b.x0 = std::clamp((int)std::floor(u0 * (float)s.tilesX), 0, s.tilesX - 1);
        b.x1 = std::clamp((int)std::floor(u1 * (float)s.tilesX), 0, s.tilesX - 1);
        b.y0 = std::clamp((int)std::floor(v0 * (float)s.tilesY), 0, s.tilesY - 1);
        b.y1 = std::clamp((int)std::floor(v1 * (float)s.tilesY), 0, s.tilesY - 1);
        b.z0 = clusterSlice(s, depth - r);
        b.z1 = clusterSlice(s, depth + r);
        for (int z = b.z0; z <= b.z1; ++z)
            for (int y = b.y0; y <= b.y1; ++y)
                for (int x = b.x0; x <= b.x1; ++x) ++counts[(size_t)((z * s.tilesY + y) * s.tilesX + x)];
    }
    // Offsets (each cluster's list, cut short if the total would run over), then the lists
    out.assign((size_t)n * 2, 0);
    size_t total = 0;
    bool complete = true;
    for (int c = 0; c < n; ++c) {
        size_t take = std::min<size_t>(counts[(size_t)c], maxIndices - std::min(total, maxIndices));
        if (take < counts[(size_t)c]) complete = false;
        out[(size_t)c * 2] = (uint32_t)total;
        counts[(size_t)c] = (uint32_t)take;   // (now: how many fit)
        total += take;
    }
    out.resize((size_t)n * 2 + total);
    std::vector<uint32_t> filled((size_t)n, 0);
    for (size_t i = 0; i < lights.size(); ++i) {
        const Box& b = boxes[i];
        for (int z = b.z0; z <= b.z1; ++z)
            for (int y = b.y0; y <= b.y1; ++y)
                for (int x = b.x0; x <= b.x1; ++x) {
                    size_t c = (size_t)((z * s.tilesY + y) * s.tilesX + x);
                    if (filled[c] >= counts[c]) continue;
                    out[(size_t)n * 2 + out[c * 2] + filled[c]++] = (uint32_t)i;
                }
    }
    for (int c = 0; c < n; ++c) out[(size_t)c * 2 + 1] = filled[(size_t)c];
    return complete;
}

std::vector<AtlasTile> allocateAtlas(int atlasSize, const std::vector<int>& sizes) {
    std::vector<AtlasTile> out(sizes.size());
    // Free squares by size; a bigger one splits into four when a smaller one is needed
    std::map<int, std::vector<AtlasTile>> free;
    free[atlasSize].push_back({0, 0, atlasSize});
    std::vector<size_t> order(sizes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sizes[a] > sizes[b]; });
    for (size_t i : order) {
        int want = sizes[i];
        if (want <= 0 || want > atlasSize) continue;
        auto it = free.lower_bound(want);
        while (it != free.end() && it->second.empty()) ++it;
        if (it == free.end()) continue;   // full
        AtlasTile t = it->second.back();
        it->second.pop_back();
        while (t.size > want) {   // split down to the size wanted, keeping the other three quarters
            int h = t.size / 2;
            free[h].push_back({t.x + h, t.y, h});
            free[h].push_back({t.x, t.y + h, h});
            free[h].push_back({t.x + h, t.y + h, h});
            t.size = h;
        }
        out[i] = t;
    }
    return out;
}
