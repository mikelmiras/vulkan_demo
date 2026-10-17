#include "render/hiz_trace.h"

#include <algorithm>

namespace hiztrace {

std::vector<std::vector<float>> nearPyramid(const std::vector<float>& row) {
    std::vector<std::vector<float>> p{row};
    while (p.back().size() > 1) {
        const std::vector<float>& below = p.back();
        std::vector<float> up((below.size() + 1) / 2);
        for (size_t i = 0; i < up.size(); ++i)
            up[i] = std::min(below[2 * i], 2 * i + 1 < below.size() ? below[2 * i + 1] : below[2 * i]);
        p.push_back(up);
    }
    return p;
}

Hit walk(const std::vector<std::vector<float>>& pyramid, int from, float z0, float dz, int top) {
    Hit h;
    const int n = (int)pyramid[0].size();
    top = std::min(top, (int)pyramid.size() - 1);
    int x = from, level = 0;
    while (x < n) {
        ++h.samples;
        const int tile = 1 << level, first = (x / tile) * tile, end = std::min(first + tile, n);
        const float deepest = z0 + dz * (float)(end - 1 - from);   // (the ray's farthest in the tile: going away)
        if (deepest < pyramid[(size_t)level][(size_t)(x / tile)]) {   // (clear: past the tile, a level up)
            x = end;
            level = std::min(level + 1, top);
            continue;
        }
        if (level > 0) {   // (something as near: smaller tiles)
            --level;
            continue;
        }
        if (z0 + dz * (float)(x - from) > pyramid[0][(size_t)x]) {
            h.at = x;
            return h;
        }
        ++x;
    }
    return h;
}

Hit march(const std::vector<float>& row, int from, float z0, float dz) {
    Hit h;
    for (int x = from; x < (int)row.size(); ++x) {
        ++h.samples;
        if (z0 + dz * (float)(x - from) > row[(size_t)x]) {
            h.at = x;
            return h;
        }
    }
    return h;
}

}  // namespace hiztrace
