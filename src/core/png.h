// Writes 8-bit RGBA images as PNG (uncompressed deflate: larger files, no zlib needed). For screenshots.
#pragma once

#include <cstdint>
#include <string>

bool writePng(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);
