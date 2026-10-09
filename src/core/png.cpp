#include "core/png.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(v >> s));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put32(out, (uint32_t)data.size());
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

}  // namespace

bool writePng(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
    // Raw scanlines: filter byte 0 (none) + RGBA pixels
    std::vector<uint8_t> raw;
    raw.reserve((size_t)height * (1 + (size_t)width * 4));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + (size_t)y * width * 4, rgba + (size_t)(y + 1) * width * 4);
    }
    // zlib stream of stored (uncompressed) deflate blocks
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;   // Adler-32
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
        pos += n;
        if (last) break;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});   // 8-bit RGBA, deflate, no filter, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    return (bool)f;
}
