// Block compression: end points along each block's main colour axis (its principal component), refined by least
// squares against the indices they give, then each pixel's nearest step. See bc_codec.h.
#include "assets/bc_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bc {

namespace {

// ---- Shared: a block's colour line ------------------------------------------------------------------------------

// The block's mean and main axis in `n` channels (power iteration on the covariance)
void principalAxis(const float (*x)[4], int n, float mean[4], float axis[4]) {
    for (int c = 0; c < 4; ++c) {
        mean[c] = 0.f;
        for (int i = 0; i < 16; ++i) mean[c] += x[i][c];
        mean[c] /= 16.f;
    }
    float cov[4][4] = {};
    for (int i = 0; i < 16; ++i)
        for (int a = 0; a < n; ++a)
            for (int b = 0; b < n; ++b) cov[a][b] += (x[i][a] - mean[a]) * (x[i][b] - mean[b]);
    float v[4] = {1.f, 1.f, 1.f, 1.f};
    for (int c = n; c < 4; ++c) v[c] = 0.f;
    for (int it = 0; it < 8; ++it) {
        float w[4] = {};
        for (int a = 0; a < n; ++a)
            for (int b = 0; b < n; ++b) w[a] += cov[a][b] * v[b];
        float len = 0.f;
        for (int a = 0; a < n; ++a) len += w[a] * w[a];
        len = std::sqrt(len);
        if (len < 1e-6f) break;   // (a flat block: any axis)
        for (int a = 0; a < n; ++a) v[a] = w[a] / len;
    }
    float len = 0.f;
    for (int a = 0; a < n; ++a) len += v[a] * v[a];
    len = std::sqrt(std::max(len, 1e-12f));
    for (int a = 0; a < 4; ++a) axis[a] = a < n ? v[a] / len : 0.f;
}

// End points: the block's extent along its axis
void lineEnds(const float (*x)[4], int n, float e0[4], float e1[4]) {
    float mean[4], axis[4];
    principalAxis(x, n, mean, axis);
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i) {
        float t = 0.f;
        for (int c = 0; c < n; ++c) t += (x[i][c] - mean[c]) * axis[c];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    for (int c = 0; c < 4; ++c) {
        e0[c] = c < n ? mean[c] + axis[c] * lo : mean[c];
        e1[c] = c < n ? mean[c] + axis[c] * hi : mean[c];
    }
}

// Least squares end points for given positions along the line (f: 0 at e0, 1 at e1); false if they're degenerate
bool refine(const float (*x)[4], const float f[16], int n, float e0[4], float e1[4]) {
    float a = 0.f, b = 0.f, c = 0.f, r0[4] = {}, r1[4] = {};
    for (int i = 0; i < 16; ++i) {
        float g = 1.f - f[i];
        a += g * g;
        b += g * f[i];
        c += f[i] * f[i];
        for (int k = 0; k < n; ++k) {
            r0[k] += g * x[i][k];
            r1[k] += f[i] * x[i][k];
        }
    }
    float det = a * c - b * b;
    if (std::fabs(det) < 1e-6f) return false;
    for (int k = 0; k < n; ++k) {
        e0[k] = std::clamp((c * r0[k] - b * r1[k]) / det, 0.f, 255.f);
        e1[k] = std::clamp((a * r1[k] - b * r0[k]) / det, 0.f, 255.f);
    }
    return true;
}

void toFloat(const uint8_t px[64], float x[16][4]) {
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) x[i][c] = px[i * 4 + c];
}

// ---- BC1 colour -------------------------------------------------------------------------------------------------

uint16_t pack565(const float c[3]) {
    int r = (int)std::lround(std::clamp(c[0], 0.f, 255.f) * 31.f / 255.f);
    int g = (int)std::lround(std::clamp(c[1], 0.f, 255.f) * 63.f / 255.f);
    int b = (int)std::lround(std::clamp(c[2], 0.f, 255.f) * 31.f / 255.f);
    return (uint16_t)(r << 11 | g << 5 | b);
}

void unpack565(uint16_t v, int out[3]) {
    int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    out[0] = r << 3 | r >> 2;
    out[1] = g << 2 | g >> 4;
    out[2] = b << 3 | b >> 2;
}

// The four colours of a 4-colour block
void palette4(uint16_t c0, uint16_t c1, int p[4][3]) {
    unpack565(c0, p[0]);
    unpack565(c1, p[1]);
    for (int k = 0; k < 3; ++k) {
        p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
        p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
    }
}

// Each pixel's nearest of the four; the summed squared error
int assign4(const float (*x)[4], uint16_t c0, uint16_t c1, uint8_t idx[16]) {
    int p[4][3];
    palette4(c0, c1, p);
    int total = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bestErr = 1 << 30;
        for (int k = 0; k < 4; ++k) {
            int err = 0;
            for (int c = 0; c < 3; ++c) {
                int d = (int)x[i][c] - p[k][c];
                err += d * d;
            }
            if (err < bestErr) { bestErr = err; best = k; }
        }
        idx[i] = (uint8_t)best;
        total += bestErr;
    }
    return total;
}

// A 4-colour block's 8 bytes (c0 > c1 picks that mode; equal end points make every pixel colour 0)
void colourBlock(const float (*x)[4], uint8_t out[8]) {
    float e0[4], e1[4];
    lineEnds(x, 3, e0, e1);
    uint16_t c0 = pack565(e1), c1 = pack565(e0);
    uint8_t idx[16];
    int err = assign4(x, c0, c1, idx);
    for (int pass = 0; pass < 2 && c0 != c1; ++pass) {   // least squares on the indices found, kept if better
        static const float kAt[4] = {0.f, 1.f, 1.f / 3.f, 2.f / 3.f};
        float f[16];
        for (int i = 0; i < 16; ++i) f[i] = kAt[idx[i]];
        float a[4], b[4];
        if (!refine(x, f, 3, a, b)) break;
        uint16_t n0 = pack565(a), n1 = pack565(b);
        uint8_t nidx[16];
        int nerr = assign4(x, n0, n1, nidx);
        if (nerr >= err) break;
        c0 = n0;
        c1 = n1;
        err = nerr;
        std::memcpy(idx, nidx, 16);
    }
    if (c0 < c1) {   // (the 4-colour mode needs c0 > c1: swap, and the indices with them)
        std::swap(c0, c1);
        static const uint8_t kSwap[4] = {1, 0, 3, 2};
        for (uint8_t& i : idx) i = kSwap[i];
    } else if (c0 == c1) {
        std::memset(idx, 0, 16);
    }
    out[0] = (uint8_t)(c0 & 0xFF);
    out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)(c1 & 0xFF);
    out[3] = (uint8_t)(c1 >> 8);
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= (uint32_t)idx[i] << (2 * i);
    for (int k = 0; k < 4; ++k) out[4 + k] = (uint8_t)(bits >> (8 * k));
}

void decodeColour(const uint8_t in[8], uint8_t px[64], bool alphaMode) {
    uint16_t c0 = (uint16_t)(in[0] | in[1] << 8), c1 = (uint16_t)(in[2] | in[3] << 8);
    int p[4][3];
    int a[4] = {255, 255, 255, 255};
    palette4(c0, c1, p);
    if (alphaMode && c0 <= c1) {   // BC1's 3-colour mode: a half-way colour and transparent black
        for (int k = 0; k < 3; ++k) {
            p[2][k] = (p[0][k] + p[1][k]) / 2;
            p[3][k] = 0;
        }
        a[3] = 0;
    }
    uint32_t bits = (uint32_t)in[4] | (uint32_t)in[5] << 8 | (uint32_t)in[6] << 16 | (uint32_t)in[7] << 24;
    for (int i = 0; i < 16; ++i) {
        int k = bits >> (2 * i) & 3;
        for (int c = 0; c < 3; ++c) px[i * 4 + c] = (uint8_t)p[k][c];
        px[i * 4 + 3] = (uint8_t)a[k];
    }
}

// ---- BC7 mode 6 -------------------------------------------------------------------------------------------------

const int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Mode6 {
    int e[2][4];   // 7-bit end points
    int p[2];      // their p-bits
    uint8_t idx[16];
};

int interp(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }

int assign16(const float (*x)[4], Mode6& m) {
    int pal[16][4];
    for (int c = 0; c < 4; ++c) {
        int a = m.e[0][c] << 1 | m.p[0], b = m.e[1][c] << 1 | m.p[1];
        for (int k = 0; k < 16; ++k) pal[k][c] = interp(a, b, kWeights4[k]);
    }
    int total = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bestErr = 1 << 30;
        for (int k = 0; k < 16; ++k) {
            int err = 0;
            for (int c = 0; c < 4; ++c) {
                int d = (int)x[i][c] - pal[k][c];
                err += d * d;
            }
            if (err < bestErr) { bestErr = err; best = k; }
        }
        m.idx[i] = (uint8_t)best;
        total += bestErr;
    }
    return total;
}

// The best quantisation of two end points over the four p-bit choices
int quantise(const float (*x)[4], const float e0[4], const float e1[4], Mode6& best) {
    int bestErr = 1 << 30;
    for (int pb = 0; pb < 4; ++pb) {
        Mode6 m;
        m.p[0] = pb & 1;
        m.p[1] = pb >> 1;
        for (int c = 0; c < 4; ++c) {
            m.e[0][c] = std::clamp((int)std::lround((e0[c] - (float)m.p[0]) / 2.f), 0, 127);
            m.e[1][c] = std::clamp((int)std::lround((e1[c] - (float)m.p[1]) / 2.f), 0, 127);
        }
        int err = assign16(x, m);
        if (err < bestErr) { bestErr = err; best = m; }
    }
    return bestErr;
}

struct BitWriter {
    uint8_t* out;
    int at = 0;
    void put(uint32_t v, int bits) {
        for (int i = 0; i < bits; ++i, ++at)
            if (v >> i & 1) out[at >> 3] |= (uint8_t)(1u << (at & 7));
    }
};

struct BitReader {
    const uint8_t* in;
    int at = 0;
    uint32_t get(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i, ++at) v |= (uint32_t)(in[at >> 3] >> (at & 7) & 1) << i;
        return v;
    }
};

}  // namespace

uint32_t blockBytes(Format f) { return f == Format::BC1 ? 8 : f == Format::RGBA8 ? 0 : 16; }

uint64_t imageBytes(Format f, uint32_t width, uint32_t height) {
    if (f == Format::RGBA8) return (uint64_t)width * height * 4;
    return (uint64_t)((width + 3) / 4) * ((height + 3) / 4) * blockBytes(f);
}

void encodeBC1(const uint8_t px[64], uint8_t out[8]) {
    float x[16][4];
    toFloat(px, x);
    colourBlock(x, out);
}

void encodeBC3(const uint8_t px[64], uint8_t out[16]) {
    // Alpha: its range in eight steps (a0 > a1), each pixel's nearest
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) {
        lo = std::min(lo, (int)px[i * 4 + 3]);
        hi = std::max(hi, (int)px[i * 4 + 3]);
    }
    int pal[8] = {hi, lo};
    for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * hi + k * lo) / 7;
    uint64_t bits = 0;
    for (int i = 0; i < 16 && hi != lo; ++i) {
        int a = px[i * 4 + 3], best = 0;
        for (int k = 1; k < 8; ++k)
            if (std::abs(pal[k] - a) < std::abs(pal[best] - a)) best = k;
        bits |= (uint64_t)best << (3 * i);
    }
    out[0] = (uint8_t)hi;
    out[1] = (uint8_t)lo;
    for (int k = 0; k < 6; ++k) out[2 + k] = (uint8_t)(bits >> (8 * k));
    float x[16][4];
    toFloat(px, x);
    colourBlock(x, out + 8);
}

void encodeBC7(const uint8_t px[64], uint8_t out[16]) {
    float x[16][4];
    toFloat(px, x);
    float e0[4], e1[4];
    lineEnds(x, 4, e0, e1);
    Mode6 m;
    int err = quantise(x, e0, e1, m);
    for (int pass = 0; pass < 2 && err > 0; ++pass) {   // least squares on the indices found, kept if better
        float f[16];
        for (int i = 0; i < 16; ++i) f[i] = (float)kWeights4[m.idx[i]] / 64.f;
        float a[4], b[4];
        if (!refine(x, f, 4, a, b)) break;
        Mode6 n;
        int nerr = quantise(x, a, b, n);
        if (nerr >= err) break;
        m = n;
        err = nerr;
    }
    if (m.idx[0] >= 8) {   // (the first index is stored without its top bit, which must be 0: swap the ends)
        for (int c = 0; c < 4; ++c) std::swap(m.e[0][c], m.e[1][c]);
        std::swap(m.p[0], m.p[1]);
        for (uint8_t& i : m.idx) i = (uint8_t)(15 - i);
    }
    std::memset(out, 0, 16);
    BitWriter w{out};
    w.put(1u << 6, 7);   // mode 6
    for (int c = 0; c < 4; ++c) {
        w.put((uint32_t)m.e[0][c], 7);
        w.put((uint32_t)m.e[1][c], 7);
    }
    w.put((uint32_t)m.p[0], 1);
    w.put((uint32_t)m.p[1], 1);
    w.put(m.idx[0], 3);
    for (int i = 1; i < 16; ++i) w.put(m.idx[i], 4);
}

void decodeBC1(const uint8_t in[8], uint8_t px[64]) { decodeColour(in, px, true); }

void decodeBC3(const uint8_t in[16], uint8_t px[64]) {
    decodeColour(in + 8, px, false);
    int a0 = in[0], a1 = in[1], pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
    } else {
        for (int k = 1; k < 5; ++k) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= (uint64_t)in[2 + k] << (8 * k);
    for (int i = 0; i < 16; ++i) px[i * 4 + 3] = (uint8_t)pal[bits >> (3 * i) & 7];
}

void decodeBC7(const uint8_t in[16], uint8_t px[64]) {
    if ((in[0] & 0x7F) != 0x40) {   // (not mode 6)
        std::memset(px, 0, 64);
        return;
    }
    BitReader r{in, 7};
    int e[2][4];
    for (int c = 0; c < 4; ++c) {
        e[0][c] = (int)r.get(7);
        e[1][c] = (int)r.get(7);
    }
    int p0 = (int)r.get(1), p1 = (int)r.get(1);
    for (int i = 0; i < 16; ++i) {
        int k = (int)r.get(i == 0 ? 3 : 4);
        for (int c = 0; c < 4; ++c) px[i * 4 + c] = (uint8_t)interp(e[0][c] << 1 | p0, e[1][c] << 1 | p1, kWeights4[k]);
    }
}

std::vector<uint8_t> encode(Format f, const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (f == Format::RGBA8) return std::vector<uint8_t>(rgba, rgba + (size_t)width * height * 4);
    std::vector<uint8_t> out((size_t)imageBytes(f, width, height));
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4, size = blockBytes(f);
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    uint32_t sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                    std::memcpy(px + (y * 4 + x) * 4, rgba + ((size_t)sy * width + sx) * 4, 4);
                }
            uint8_t* block = &out[((size_t)by * bw + bx) * size];
            if (f == Format::BC1) encodeBC1(px, block);
            else if (f == Format::BC3) encodeBC3(px, block);
            else encodeBC7(px, block);
        }
    return out;
}

std::vector<uint8_t> decode(Format f, const uint8_t* blocks, uint32_t width, uint32_t height) {
    if (f == Format::RGBA8) return std::vector<uint8_t>(blocks, blocks + (size_t)width * height * 4);
    std::vector<uint8_t> out((size_t)width * height * 4);
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4, size = blockBytes(f);
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* block = blocks + ((size_t)by * bw + bx) * size;
            if (f == Format::BC1) decodeBC1(block, px);
            else if (f == Format::BC3) decodeBC3(block, px);
            else decodeBC7(block, px);
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::memcpy(&out[(((size_t)by * 4 + y) * width + bx * 4 + x) * 4], px + (y * 4 + x) * 4, 4);
        }
    return out;
}

}  // namespace bc
