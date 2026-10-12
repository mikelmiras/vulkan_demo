// A small LZ4-style compressor (the LZ4 block format: literal runs and back-references within 64 KB), for packs:
// fast to unpack, good on scenes, models and shaders, while already-compressed files (PNG, MP3...) are best left
// alone.
#pragma once

#include <cstddef>
#include <string>

namespace lz {

std::string compress(const char* data, size_t size);
// `size`: the original size (stored next to the data). False if the data is damaged.
bool decompress(const char* data, size_t dataSize, std::string& out, size_t size);

}  // namespace lz
