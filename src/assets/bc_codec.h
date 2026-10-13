// Block compression for textures (the formats every desktop GPU reads directly): BC1 (opaque colour, 4 bits a
// pixel), BC3 (colour and alpha, 8 bits) and BC7 (colour and alpha at better quality, 8 bits; this encoder uses its
// mode 6 only: one pair of RGBA end points a block, 16 steps between them). Each 4 x 4 block of pixels becomes 8 or
// 16 bytes. Plain functions: export's cooking (assets/cooking.h) encodes, the renderer decodes for GPUs without BC
// support, tests check both.
#pragma once

#include <cstdint>
#include <vector>

namespace bc {

enum class Format : uint8_t { RGBA8 = 0, BC1 = 1, BC3 = 2, BC7 = 3 };

// Bytes of a 4 x 4 block (0 for RGBA8, which isn't in blocks)
uint32_t blockBytes(Format f);
// Bytes of a w x h image (a partial block at the edges takes a whole one)
uint64_t imageBytes(Format f, uint32_t width, uint32_t height);

// RGBA8 pixels (w x h) -> blocks, row by row (edge blocks repeat the last row / column)
std::vector<uint8_t> encode(Format f, const uint8_t* rgba, uint32_t width, uint32_t height);
// ... and back
std::vector<uint8_t> decode(Format f, const uint8_t* blocks, uint32_t width, uint32_t height);

// One block: 16 RGBA pixels (row by row) <-> its bytes
void encodeBC1(const uint8_t px[64], uint8_t out[8]);
void encodeBC3(const uint8_t px[64], uint8_t out[16]);
void encodeBC7(const uint8_t px[64], uint8_t out[16]);
void decodeBC1(const uint8_t in[8], uint8_t px[64]);
void decodeBC3(const uint8_t in[16], uint8_t px[64]);
// BC7 mode 6 blocks only, what this encoder makes (blocks in the other modes decode as transparent black)
void decodeBC7(const uint8_t in[16], uint8_t px[64]);

}  // namespace bc
