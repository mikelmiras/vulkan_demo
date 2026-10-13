#include "core/hdr_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/vfs.h"

bool writeRadianceHdr(const std::string& path, uint32_t width, uint32_t height, const float* rgb) {
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    if (!out) return false;
    out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << "\n";
    std::vector<uint8_t> row((size_t)width * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float* p = rgb + ((size_t)y * width + x) * 3;
            float r = std::max(p[0], 0.f), g = std::max(p[1], 0.f), b = std::max(p[2], 0.f);
            float m = std::max(r, std::max(g, b));
            uint8_t* e = &row[(size_t)x * 4];
            if (!(m >= 1e-32f) || !std::isfinite(m)) {   // (black, or not a number: black)
                e[0] = e[1] = e[2] = e[3] = 0;
                continue;
            }
            int exponent = 0;
            float scale = std::frexp(m, &exponent) * 256.f / m;   // m = mantissa * 2^exponent, mantissa in [0.5, 1)
            e[0] = (uint8_t)std::min(r * scale, 255.f);
            e[1] = (uint8_t)std::min(g * scale, 255.f);
            e[2] = (uint8_t)std::min(b * scale, 255.f);
            e[3] = (uint8_t)std::clamp(exponent + 128, 0, 255);
        }
        out.write((const char*)row.data(), (std::streamsize)row.size());
    }
    return (bool)out;
}

bool readRadianceHdr(const std::string& path, uint32_t& width, uint32_t& height, std::vector<float>& rgb) {
    std::string bytes;
    if (!vfs::read(std::filesystem::u8path(path), bytes) || bytes.rfind("#?", 0) != 0) return false;
    size_t blank = bytes.find("\n\n");
    if (blank == std::string::npos) return false;
    size_t lineEnd = bytes.find('\n', blank + 2);
    if (lineEnd == std::string::npos) return false;
    unsigned h = 0, w = 0;
    if (std::sscanf(bytes.substr(blank + 2, lineEnd - blank - 2).c_str(), "-Y %u +X %u", &h, &w) != 2) return false;
    const uint8_t* data = (const uint8_t*)bytes.data() + lineEnd + 1;
    if (bytes.size() - (lineEnd + 1) < (size_t)w * h * 4) return false;
    width = w;
    height = h;
    rgb.resize((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint8_t* e = data + i * 4;
        float f = std::ldexp(1.f, (int)e[3] - (128 + 8));
        for (int c = 0; c < 3; ++c) rgb[i * 3 + (size_t)c] = e[3] ? ((float)e[c] + 0.5f) * f : 0.f;
    }
    return true;
}

float halfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023, bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {   // subnormal: normalise it
            int e = -1;
            do { ++e; mantissa <<= 1; } while (!(mantissa & 1024));
            bits = sign | (uint32_t)(127 - 15 - e) << 23 | (mantissa & 1023) << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | mantissa << 13;
    } else {
        bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
