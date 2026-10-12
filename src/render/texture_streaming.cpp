#include "render/texture_streaming.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace streaming {

uint32_t mipCount(uint32_t width, uint32_t height) {
    uint32_t mips = 1;
    while ((std::max(width, height) >> mips) > 0) ++mips;
    return mips;
}

uint64_t chainBytes(uint32_t width, uint32_t height, uint32_t first, uint32_t mips) {
    uint64_t bytes = 0;
    for (uint32_t m = first; m < mips; ++m)
        bytes += (uint64_t)std::max(width >> m, 1u) * std::max(height >> m, 1u) * 4;
    return bytes;
}

uint32_t levelForSize(uint32_t width, uint32_t height, uint32_t size) {
    uint32_t level = 0;
    while ((std::max(width, height) >> level) > size) ++level;
    return level;
}

std::vector<uint32_t> plan(const std::vector<TextureState>& textures, const Settings& settings, double now) {
    const size_t n = textures.size();
    std::vector<uint32_t> target(n), floorLevel(n);
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        const TextureState& t = textures[i];
        uint32_t last = t.mips > 0 ? t.mips - 1 : 0;
        floorLevel[i] = t.pinned ? 0 : std::min(levelForSize(t.width, t.height, settings.startSize), last);
        bool recent = t.wanted < t.mips && now - t.lastWanted <= settings.forgetAfter;
        target[i] = t.pinned ? 0 : recent ? std::min(t.wanted, floorLevel[i]) : floorLevel[i];
        total += chainBytes(t.width, t.height, target[i], t.mips);
    }
    if (total <= settings.budget) return target;
    // Over budget: the least recently needed first (then the largest), a level at a time, round after round
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (textures[a].lastWanted != textures[b].lastWanted) return textures[a].lastWanted < textures[b].lastWanted;
        return chainBytes(textures[a].width, textures[a].height, target[a], textures[a].mips) >
               chainBytes(textures[b].width, textures[b].height, target[b], textures[b].mips);
    });
    for (bool dropped = true; total > settings.budget && dropped;) {
        dropped = false;
        for (size_t i : order) {
            if (total <= settings.budget) break;
            const TextureState& t = textures[i];
            if (t.pinned || target[i] >= floorLevel[i]) continue;
            uint64_t before = chainBytes(t.width, t.height, target[i], t.mips);
            ++target[i];
            total -= before - chainBytes(t.width, t.height, target[i], t.mips);
            dropped = true;
        }
    }
    return target;
}

namespace {

float toLinear(uint8_t c) {
    float v = (float)c / 255.f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

uint8_t toSrgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(s * 255.f);
}

}  // namespace

std::vector<uint8_t> downsample(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t level, uint32_t& outWidth,
                                uint32_t& outHeight) {
    outWidth = width;
    outHeight = height;
    std::vector<uint8_t> out(rgba, rgba + (size_t)width * height * 4);
    if (level == 0) return out;
    static float lut[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) lut[i] = toLinear((uint8_t)i);
        ready = true;
    }
    for (uint32_t l = 0; l < level && (outWidth > 1 || outHeight > 1); ++l) {
        uint32_t w = std::max(outWidth / 2, 1u), h = std::max(outHeight / 2, 1u);
        std::vector<uint8_t> next((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t x0 = std::min(x * 2, outWidth - 1), x1 = std::min(x * 2 + 1, outWidth - 1);
                uint32_t y0 = std::min(y * 2, outHeight - 1), y1 = std::min(y * 2 + 1, outHeight - 1);
                const uint8_t* p[4] = {&out[((size_t)y0 * outWidth + x0) * 4], &out[((size_t)y0 * outWidth + x1) * 4],
                                       &out[((size_t)y1 * outWidth + x0) * 4], &out[((size_t)y1 * outWidth + x1) * 4]};
                uint8_t* d = &next[((size_t)y * w + x) * 4];
                for (int c = 0; c < 3; ++c) d[c] = toSrgb((lut[p[0][c]] + lut[p[1][c]] + lut[p[2][c]] + lut[p[3][c]]) * 0.25f);
                d[3] = (uint8_t)((p[0][3] + p[1][3] + p[2][3] + p[3][3] + 2) / 4);
            }
        out.swap(next);
        outWidth = w;
        outHeight = h;
    }
    return out;
}

}  // namespace streaming
