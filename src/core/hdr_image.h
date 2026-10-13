// Radiance .hdr images (RGBE: an 8-bit mantissa per channel sharing an exponent; about 1% precision over a huge
// range): HDR screenshots write them, and the sky loader (stb_image) reads them. Flat scanlines, no run-length
// compression.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// `rgb`: width x height x 3 floats, top row first (linear light)
bool writeRadianceHdr(const std::string& path, uint32_t width, uint32_t height, const float* rgb);
// ... and back (flat files only, as written above)
bool readRadianceHdr(const std::string& path, uint32_t& width, uint32_t& height, std::vector<float>& rgb);
// IEEE half float -> float (render targets read back)
float halfToFloat(uint16_t h);
