// Texture streaming: which mip levels of each texture live on the GPU. A texture's finest levels are uploaded only
// once something on screen needs them (the scene shader reports the finest level it would sample, per texture), and
// under a memory budget the textures needed least lose their finest levels first. The CPU keeps every texture whole
// (TextureLibrary); this decides what the GPU holds. Plain functions: the renderer (renderer_textures.cpp) drives
// them, tests check them.
#pragma once

#include <cstdint>
#include <vector>

namespace streaming {

struct TextureState {
    uint32_t width = 0, height = 0;   // the full image
    uint32_t mips = 1;                // levels in its full chain
    uint32_t resident = 0;            // the finest level on the GPU now
    uint32_t wanted = 0;              // the finest level asked for lately (mips: none)
    double lastWanted = -1e9;         // when it last was (seconds)
    bool pinned = false;              // always whole (UI images, terrains, textures without mipmaps)
    uint32_t blockBytes = 0;          // block compressed (cooked): bytes a 4 x 4 block; 0: RGBA8
};

// Levels in a full chain for a w x h image (with mipmaps)
uint32_t mipCount(uint32_t width, uint32_t height);
// Bytes of a chain from level `first` down to 1 x 1: RGBA8, or `blockBytes` a 4 x 4 block (block compressed)
uint64_t chainBytes(uint32_t width, uint32_t height, uint32_t first, uint32_t mips, uint32_t blockBytes = 0);
// The finest level whose larger side is at most `size` (where streamed textures start)
uint32_t levelForSize(uint32_t width, uint32_t height, uint32_t size);

struct Settings {
    uint64_t budget = 512ull << 20;   // bytes of textures on the GPU
    uint32_t startSize = 256;         // never coarser than this (larger side); new textures start here
    double forgetAfter = 3.0;         // seconds without being asked for: back to startSize
};

// The finest level each texture should have resident: what's wanted (pinned ones: level 0; none asked for lately:
// the start level), then while the total is over budget, the textures asked for longest ago (then the largest)
// give up their finest level, never past the start level
std::vector<uint32_t> plan(const std::vector<TextureState>& textures, const Settings& settings, double now);

// RGBA8 sRGB pixels at mip `level` of a w x h image, box-filtered (in linear light) level by level. `data`: the
// bytes are numbers (a normal, metal / rough or height map), averaged as they are: a normal map's averaged normals
// come out shorter where they disagree, which specular anti-aliasing reads as roughness
std::vector<uint8_t> downsample(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t level, uint32_t& outWidth,
                                uint32_t& outHeight, bool data = false);

}  // namespace streaming
