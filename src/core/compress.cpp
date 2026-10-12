#include "core/compress.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace lz {
namespace {

constexpr size_t kMinMatch = 4, kLastLiterals = 5, kMatchSafety = 12, kMaxOffset = 65535;
constexpr int kHashBits = 16;

uint32_t read32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint32_t hash(uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }

// A length beyond what the token holds: 255s, then the rest
void putLength(std::string& out, size_t n) {
    while (n >= 255) {
        out += (char)255;
        n -= 255;
    }
    out += (char)n;
}

void putSequence(std::string& out, const char* lit, size_t litLen, size_t offset, size_t matchLen) {
    size_t ml = matchLen ? matchLen - kMinMatch : 0;
    uint8_t token = (uint8_t)((litLen >= 15 ? 15 : litLen) << 4) | (uint8_t)(matchLen ? (ml >= 15 ? 15 : ml) : 0);
    out += (char)token;
    if (litLen >= 15) putLength(out, litLen - 15);
    out.append(lit, litLen);
    if (!matchLen) return;
    out += (char)(offset & 0xFF);
    out += (char)(offset >> 8);
    if (ml >= 15) putLength(out, ml - 15);
}

}  // namespace

std::string compress(const char* data, size_t size) {
    std::string out;
    out.reserve(size / 2 + 16);
    if (size < kMatchSafety + 1) {   // too small for any match: all literals
        putSequence(out, data, size, 0, 0);
        return out;
    }
    std::vector<int64_t> table((size_t)1 << kHashBits, -1);
    size_t anchor = 0, i = 0;
    const size_t limit = size - kMatchSafety;   // matches start before here (the end is always literals)
    while (i < limit) {
        uint32_t v = read32(data + i);
        uint32_t h = hash(v);
        int64_t cand = table[h];
        table[h] = (int64_t)i;
        if (cand < 0 || i - (size_t)cand > kMaxOffset || read32(data + cand) != v) {
            ++i;
            continue;
        }
        // Extend the match (it must end kLastLiterals before the end)
        size_t len = kMinMatch;
        while (i + len < size - kLastLiterals && data[(size_t)cand + len] == data[i + len]) ++len;
        putSequence(out, data + anchor, i - anchor, i - (size_t)cand, len);
        i += len;
        anchor = i;
    }
    putSequence(out, data + anchor, size - anchor, 0, 0);   // the rest, as literals
    return out;
}

bool decompress(const char* in, size_t inSize, std::string& out, size_t size) {
    out.resize(size);
    char* dst = out.data();
    size_t ip = 0, op = 0;
    auto length = [&](size_t n) -> int64_t {   // a length that continues in 255s
        if (n != 15) return (int64_t)n;
        for (;;) {
            if (ip >= inSize) return -1;
            uint8_t b = (uint8_t)in[ip++];
            n += b;
            if (b != 255) return (int64_t)n;
        }
    };
    while (ip < inSize) {
        uint8_t token = (uint8_t)in[ip++];
        int64_t lit = length(token >> 4);
        if (lit < 0 || ip + (size_t)lit > inSize || op + (size_t)lit > size) return false;
        std::memcpy(dst + op, in + ip, (size_t)lit);
        ip += (size_t)lit;
        op += (size_t)lit;
        if (ip >= inSize) break;   // the last sequence: literals only
        if (ip + 2 > inSize) return false;
        size_t offset = (uint8_t)in[ip] | ((size_t)(uint8_t)in[ip + 1] << 8);
        ip += 2;
        int64_t ml = length(token & 15);
        if (ml < 0 || offset == 0 || offset > op) return false;
        size_t len = (size_t)ml + kMinMatch;
        if (op + len > size) return false;
        for (size_t k = 0; k < len; ++k, ++op) dst[op] = dst[op - offset];   // (may overlap: byte by byte)
    }
    return op == size;
}

}  // namespace lz
