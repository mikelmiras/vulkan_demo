// X25519 after TweetNaCl (16-bit limbs, constant-time ladder), Poly1305 after poly1305-donna (26-bit limbs),
// ChaCha20 and BLAKE2s straight from their RFCs
#include "net/crypto.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace crypto {

namespace {

uint32_t load32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void store32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
void store64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
uint32_t rotl(uint32_t v, int n) { return v << n | v >> (32 - n); }
uint32_t rotr(uint32_t v, int n) { return v >> n | v << (32 - n); }

}  // namespace

void randomBytes(void* out, size_t size) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("no secure random numbers");
#elif defined(__APPLE__)
    arc4random_buf(out, size);
#else
    auto* p = static_cast<uint8_t*>(out);
    while (size > 0) {
        ssize_t n = getrandom(p, size, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("no secure random numbers");
        }
        p += n;
        size -= (size_t)n;
    }
#endif
}

bool equal(const void* a, const void* b, size_t size) {
    const auto* x = static_cast<const volatile uint8_t*>(a);
    const auto* y = static_cast<const volatile uint8_t*>(b);
    uint8_t d = 0;
    for (size_t i = 0; i < size; ++i) d |= x[i] ^ y[i];
    return d == 0;
}

// ---------------------------------------------------------------------------------------------
// X25519: field elements mod 2^255 - 19 as 16 limbs of 16 bits (in 64-bit integers, room for products)
// ---------------------------------------------------------------------------------------------
namespace {

using Fe = int64_t[16];

void carry(Fe o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1;
        else o[0] += 38 * (c - 1);   // (2^256 = 38 mod p)
        o[i] -= c * 65536;
    }
}

void select(Fe p, Fe q, int64_t b) {   // swaps p and q when b is 1, without branching
    int64_t mask = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(uint8_t* o, const Fe n) {
    Fe m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {   // subtract p while that stays positive
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

void unpack(Fe o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

void add(Fe o, const Fe a, const Fe b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
void sub(Fe o, const Fe a, const Fe b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
void mul(Fe o, const Fe a, const Fe b) {
    int64_t t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    carry(o);
    carry(o);
}
void square(Fe o, const Fe a) { mul(o, a, a); }
void invert(Fe o, const Fe in) {   // in^(p - 2)
    Fe c;
    for (int i = 0; i < 16; ++i) c[i] = in[i];
    for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4) mul(c, c, in);
    }
    for (int i = 0; i < 16; ++i) o[i] = c[i];
}

}  // namespace

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    static const Fe k121665 = {0xDB41, 1};
    uint8_t z[32];
    std::memcpy(z, scalar, 32);
    z[31] = (uint8_t)((z[31] & 127) | 64);   // (clamped: RFC 7748)
    z[0] &= 248;
    Fe x, a, b, c, d, e, f;
    unpack(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {   // the Montgomery ladder
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        select(a, b, r);
        select(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        square(d, e);
        square(f, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        square(b, a);
        sub(c, d, f);
        mul(a, c, k121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        square(b, e);
        select(a, b, r);
        select(c, d, r);
    }
    invert(c, c);
    mul(a, a, c);
    pack(out, a);
}

// ---------------------------------------------------------------------------------------------
// Ed25519 (after TweetNaCl): points in extended coordinates (X, Y, Z, T) on the twisted Edwards curve
// ---------------------------------------------------------------------------------------------
namespace {

const Fe kZero = {}, kOne = {1};
const Fe kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const Fe kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const Fe kBaseX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const Fe kBaseY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const Fe kSqrtM1 = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// The group's order, L = 2^252 + 27742317777372353535851937790883648493, little-endian bytes
const int64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

void copyFe(Fe o, const Fe a) {
    for (int i = 0; i < 16; ++i) o[i] = a[i];
}
bool feDiffers(const Fe a, const Fe b) {
    uint8_t x[32], y[32];
    pack(x, a);
    pack(y, b);
    return !equal(x, y, 32);
}
uint8_t parity(const Fe a) {
    uint8_t d[32];
    pack(d, a);
    return d[0] & 1;
}
void pow2523(Fe o, const Fe in) {   // in^((p - 5) / 8)
    Fe c;
    copyFe(c, in);
    for (int a = 250; a >= 0; --a) {
        square(c, c);
        if (a != 1) mul(c, c, in);
    }
    copyFe(o, c);
}

using Point = Fe[4];

void pointAdd(Point p, Point q) {   // p += q
    Fe a, b, c, d, t, e, f, g, h;
    sub(a, p[1], p[0]);
    sub(t, q[1], q[0]);
    mul(a, a, t);
    add(b, p[0], p[1]);
    add(t, q[0], q[1]);
    mul(b, b, t);
    mul(c, p[3], q[3]);
    mul(c, c, kD2);
    mul(d, p[2], q[2]);
    add(d, d, d);
    sub(e, b, a);
    sub(f, d, c);
    add(g, d, c);
    add(h, b, a);
    mul(p[0], e, f);
    mul(p[1], h, g);
    mul(p[2], g, f);
    mul(p[3], e, h);
}

void pointSwap(Point p, Point q, int64_t b) {
    for (int i = 0; i < 4; ++i) select(p[i], q[i], b);
}

void pointPack(uint8_t* r, Point p) {
    Fe tx, ty, zi;
    invert(zi, p[2]);
    mul(tx, p[0], zi);
    mul(ty, p[1], zi);
    pack(r, ty);
    r[31] ^= (uint8_t)(parity(tx) << 7);
}

void scalarMult(Point p, Point q, const uint8_t* s) {   // p = s * q (q is spent)
    copyFe(p[0], kZero);
    copyFe(p[1], kOne);
    copyFe(p[2], kOne);
    copyFe(p[3], kZero);
    for (int i = 255; i >= 0; --i) {
        int64_t b = (s[i / 8] >> (i & 7)) & 1;
        pointSwap(p, q, b);
        pointAdd(q, p);
        pointAdd(p, p);
        pointSwap(p, q, b);
    }
}

void scalarBase(Point p, const uint8_t* s) {
    Point q;
    copyFe(q[0], kBaseX);
    copyFe(q[1], kBaseY);
    copyFe(q[2], kOne);
    mul(q[3], kBaseX, kBaseY);
    scalarMult(p, q, s);
}

// x (64 limbs of a byte or more) mod L, into 32 bytes
void modL(uint8_t* r, int64_t x[64]) {
    int64_t carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * kL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * kL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * kL[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

void reduce(uint8_t* r) {   // 64 bytes mod L, into the first 32
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = r[i];
    for (int i = 0; i < 64; ++i) r[i] = 0;
    modL(r, x);
}

// The point a public key encodes, negated (what verifying wants); false if it isn't one
bool unpackNegated(Point r, const uint8_t p[32]) {
    Fe t, chk, num, den, den2, den4, den6;
    copyFe(r[2], kOne);
    unpack(r[1], p);
    square(num, r[1]);
    mul(den, num, kD);
    sub(num, num, r[2]);
    add(den, r[2], den);
    square(den2, den);
    square(den4, den2);
    mul(den6, den4, den2);
    mul(t, den6, num);
    mul(t, t, den);
    pow2523(t, t);
    mul(t, t, num);
    mul(t, t, den);
    mul(t, t, den);
    mul(r[0], t, den);
    square(chk, r[0]);
    mul(chk, chk, den);
    if (feDiffers(chk, num)) mul(r[0], r[0], kSqrtM1);
    square(chk, r[0]);
    mul(chk, chk, den);
    if (feDiffers(chk, num)) return false;
    if (parity(r[0]) == (p[31] >> 7)) sub(r[0], kZero, r[0]);
    mul(r[3], r[0], r[1]);
    return true;
}

// The secret scalar and the nonce prefix from a seed (SHA-512, clamped)
std::array<uint8_t, 64> expand(const uint8_t seed[32]) {
    std::array<uint8_t, 64> d = sha512(seed, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
    return d;
}

}  // namespace

SigningKey SigningKey::fromSeed(const uint8_t seed[32]) {
    SigningKey k;
    std::memcpy(k.seed.data(), seed, 32);
    std::array<uint8_t, 64> d = expand(seed);
    Point p;
    scalarBase(p, d.data());
    pointPack(k.publicKey.data(), p);
    return k;
}

SigningKey SigningKey::generate() {
    uint8_t seed[32];
    randomBytes(seed, sizeof(seed));
    return fromSeed(seed);
}

std::array<uint8_t, 64> sign(const SigningKey& key, const void* message, size_t size) {
    std::array<uint8_t, 64> d = expand(key.seed.data());
    std::vector<uint8_t> buf(64 + size);   // R | A | message (R's room first holds the nonce prefix)
    std::memcpy(buf.data() + 64, message, size);
    std::memcpy(buf.data() + 32, d.data() + 32, 32);
    std::array<uint8_t, 64> r = sha512(buf.data() + 32, size + 32);
    reduce(r.data());
    Point p;
    scalarBase(p, r.data());
    pointPack(buf.data(), p);
    std::memcpy(buf.data() + 32, key.publicKey.data(), 32);
    std::array<uint8_t, 64> h = sha512(buf.data(), size + 64);
    reduce(h.data());
    int64_t x[64] = {};
    for (int i = 0; i < 32; ++i) x[i] = r[(size_t)i];
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 32; ++j) x[i + j] += (int64_t)h[(size_t)i] * d[(size_t)j];
    std::array<uint8_t, 64> sig;
    std::memcpy(sig.data(), buf.data(), 32);
    modL(sig.data() + 32, x);
    return sig;
}

bool verify(const uint8_t publicKey[32], const void* message, size_t size, const uint8_t signature[64]) {
    // S must be below L (no other encodings of the same signature)
    for (int i = 31; i >= 0; --i) {
        if (signature[32 + i] < kL[i]) break;
        if (signature[32 + i] > kL[i] || i == 0) return false;
    }
    Point q;
    if (!unpackNegated(q, publicKey)) return false;
    std::vector<uint8_t> buf(64 + size);
    std::memcpy(buf.data(), signature, 32);
    std::memcpy(buf.data() + 32, publicKey, 32);
    std::memcpy(buf.data() + 64, message, size);
    std::array<uint8_t, 64> h = sha512(buf.data(), buf.size());
    reduce(h.data());
    Point p, base;
    scalarMult(p, q, h.data());
    scalarBase(base, signature + 32);
    pointAdd(p, base);
    uint8_t t[32];
    pointPack(t, p);
    return equal(t, signature, 32);
}

std::string toHex(const void* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    const auto* p = static_cast<const uint8_t*>(data);
    std::string s;
    for (size_t i = 0; i < size; ++i) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

bool fromHex(const std::string& text, uint8_t* out, size_t size) {
    if (text.size() != size * 2) return false;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < size; ++i) {
        int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

KeyPair KeyPair::generate() {
    static const uint8_t kBase[32] = {9};
    KeyPair k;
    randomBytes(k.secret.data(), 32);
    x25519(k.publicKey.data(), k.secret.data(), kBase);
    return k;
}

bool sharedSecret(const KeyPair& mine, const Key& theirs, Key& out) {
    x25519(out.data(), mine.secret.data(), theirs.data());
    static const uint8_t kZero[32] = {};
    return !equal(out.data(), kZero, 32);
}

// ---------------------------------------------------------------------------------------------
// SHA-512
// ---------------------------------------------------------------------------------------------
namespace {

const uint64_t kK512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

uint64_t rotr64(uint64_t v, int n) { return v >> n | v << (64 - n); }

void sha512Block(uint64_t h[8], const uint8_t* p) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = 0;
        for (int k = 0; k < 8; ++k) w[i] = w[i] << 8 | p[i * 8 + k];
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t t1 = k + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + kK512[i] + w[i];
        uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

}  // namespace

std::array<uint8_t, 64> sha512(const void* data, size_t size) {
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                     0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    const auto* p = static_cast<const uint8_t*>(data);
    size_t whole = size / 128 * 128;
    for (size_t at = 0; at < whole; at += 128) sha512Block(h, p + at);
    uint8_t last[256] = {};
    size_t rest = size - whole;
    std::memcpy(last, p + whole, rest);
    last[rest] = 0x80;
    size_t blocks = rest + 1 + 16 > 128 ? 2 : 1;
    uint64_t bits = (uint64_t)size * 8;
    for (int i = 0; i < 8; ++i) last[blocks * 128 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t b = 0; b < blocks; ++b) sha512Block(h, last + b * 128);
    std::array<uint8_t, 64> out;
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 8; ++k) out[(size_t)(i * 8 + k)] = (uint8_t)(h[i] >> (56 - 8 * k));
    return out;
}

// ---------------------------------------------------------------------------------------------
// BLAKE2s
// ---------------------------------------------------------------------------------------------
namespace {

const uint32_t kIv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
const uint8_t kSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

}  // namespace

Blake2s::Blake2s(size_t outSize, const void* key, size_t keySize) : outSize_(outSize) {
    if (outSize_ < 1 || outSize_ > 32) outSize_ = 32;
    if (keySize > 32) keySize = 32;
    for (int i = 0; i < 8; ++i) h_[i] = kIv[i];
    h_[0] ^= 0x01010000u ^ (uint32_t)(keySize << 8) ^ (uint32_t)outSize_;
    std::memset(buf_, 0, sizeof(buf_));
    if (keySize > 0) {   // (the key is the first block, padded)
        std::memcpy(buf_, key, keySize);
        used_ = 64;
    }
}

void Blake2s::compress(bool last) {
    uint32_t v[16], m[16];
    for (int i = 0; i < 8; ++i) {
        v[i] = h_[i];
        v[i + 8] = kIv[i];
    }
    v[12] ^= t_[0];
    v[13] ^= t_[1];
    if (last) v[14] = ~v[14];
    for (int i = 0; i < 16; ++i) m[i] = load32(buf_ + 4 * i);
    auto g = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
        v[a] = v[a] + v[b] + x;
        v[d] = rotr(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = rotr(v[b] ^ v[c], 12);
        v[a] = v[a] + v[b] + y;
        v[d] = rotr(v[d] ^ v[a], 8);
        v[c] = v[c] + v[d];
        v[b] = rotr(v[b] ^ v[c], 7);
    };
    for (const auto& s : kSigma) {
        g(0, 4, 8, 12, m[s[0]], m[s[1]]);
        g(1, 5, 9, 13, m[s[2]], m[s[3]]);
        g(2, 6, 10, 14, m[s[4]], m[s[5]]);
        g(3, 7, 11, 15, m[s[6]], m[s[7]]);
        g(0, 5, 10, 15, m[s[8]], m[s[9]]);
        g(1, 6, 11, 12, m[s[10]], m[s[11]]);
        g(2, 7, 8, 13, m[s[12]], m[s[13]]);
        g(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; ++i) h_[i] ^= v[i] ^ v[i + 8];
}

Blake2s& Blake2s::update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        if (used_ == 64) {   // (a full block is only compressed once more comes: the last one is flagged)
            t_[0] += 64;
            if (t_[0] < 64) ++t_[1];
            compress(false);
            used_ = 0;
        }
        size_t n = std::min(size, 64 - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n;
        p += n;
        size -= n;
    }
    return *this;
}

void Blake2s::final(uint8_t* out) {
    t_[0] += (uint32_t)used_;
    if (t_[0] < used_) ++t_[1];
    std::memset(buf_ + used_, 0, 64 - used_);
    compress(true);
    uint8_t full[32];
    for (int i = 0; i < 8; ++i) store32(full + 4 * i, h_[i]);
    std::memcpy(out, full, outSize_);
}

Key hash(const void* data, size_t size) {
    Key k;
    Blake2s().update(data, size).final(k.data());
    return k;
}
Key hash(const std::string& s) { return hash(s.data(), s.size()); }

Key mac(const Key& key, const void* data, size_t size) {
    Key k;
    Blake2s(32, key.data(), key.size()).update(data, size).final(k.data());
    return k;
}

// ---------------------------------------------------------------------------------------------
// ChaCha20 and Poly1305
// ---------------------------------------------------------------------------------------------
namespace {

void chachaBlock(const Key& key, uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) s[4 + i] = load32(key.data() + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = load32(nonce + 4 * i);
    uint32_t x[16];
    std::memcpy(x, s, sizeof(s));
    auto qr = [&](int a, int b, int c, int d) {
        x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16);
        x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12);
        x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8);
        x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        qr(0, 4, 8, 12);
        qr(1, 5, 9, 13);
        qr(2, 6, 10, 14);
        qr(3, 7, 11, 15);
        qr(0, 5, 10, 15);
        qr(1, 6, 11, 12);
        qr(2, 7, 8, 13);
        qr(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) store32(out + 4 * i, x[i] + s[i]);
}

}  // namespace

void chacha20(const Key& key, uint32_t counter, const uint8_t nonce[12], uint8_t* data, size_t size) {
    uint8_t block[64];
    for (size_t at = 0; at < size; at += 64, ++counter) {
        chachaBlock(key, counter, nonce, block);
        size_t n = std::min<size_t>(64, size - at);
        for (size_t i = 0; i < n; ++i) data[at + i] ^= block[i];
    }
}

namespace {

struct Poly1305 {
    uint32_t r[5], s[4], h[5] = {0, 0, 0, 0, 0};
    uint32_t pad[4];
    uint8_t buf[16];
    size_t used = 0;

    explicit Poly1305(const uint8_t key[32]) {
        r[0] = load32(key + 0) & 0x3ffffff;
        r[1] = (load32(key + 3) >> 2) & 0x3ffff03;
        r[2] = (load32(key + 6) >> 4) & 0x3ffc0ff;
        r[3] = (load32(key + 9) >> 6) & 0x3f03fff;
        r[4] = (load32(key + 12) >> 8) & 0x00fffff;
        for (int i = 0; i < 4; ++i) {
            s[i] = r[i + 1] * 5;
            pad[i] = load32(key + 16 + 4 * i);
        }
    }
    void block(const uint8_t* m, uint32_t hibit) {
        const uint32_t mask = 0x3ffffff;
        h[0] += load32(m + 0) & mask;
        h[1] += (load32(m + 3) >> 2) & mask;
        h[2] += (load32(m + 6) >> 4) & mask;
        h[3] += (load32(m + 9) >> 6) & mask;
        h[4] += (load32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h[0] * r[0] + (uint64_t)h[1] * s[3] + (uint64_t)h[2] * s[2] + (uint64_t)h[3] * s[1] + (uint64_t)h[4] * s[0];
        uint64_t d1 = (uint64_t)h[0] * r[1] + (uint64_t)h[1] * r[0] + (uint64_t)h[2] * s[3] + (uint64_t)h[3] * s[2] + (uint64_t)h[4] * s[1];
        uint64_t d2 = (uint64_t)h[0] * r[2] + (uint64_t)h[1] * r[1] + (uint64_t)h[2] * r[0] + (uint64_t)h[3] * s[3] + (uint64_t)h[4] * s[2];
        uint64_t d3 = (uint64_t)h[0] * r[3] + (uint64_t)h[1] * r[2] + (uint64_t)h[2] * r[1] + (uint64_t)h[3] * r[0] + (uint64_t)h[4] * s[3];
        uint64_t d4 = (uint64_t)h[0] * r[4] + (uint64_t)h[1] * r[3] + (uint64_t)h[2] * r[2] + (uint64_t)h[3] * r[1] + (uint64_t)h[4] * r[0];
        uint64_t c = d0 >> 26; h[0] = (uint32_t)d0 & mask;
        d1 += c; c = d1 >> 26; h[1] = (uint32_t)d1 & mask;
        d2 += c; c = d2 >> 26; h[2] = (uint32_t)d2 & mask;
        d3 += c; c = d3 >> 26; h[3] = (uint32_t)d3 & mask;
        d4 += c; c = d4 >> 26; h[4] = (uint32_t)d4 & mask;
        h[0] += (uint32_t)c * 5;
        h[1] += h[0] >> 26;
        h[0] &= mask;
    }
    void update(const uint8_t* m, size_t n) {
        while (n > 0) {
            size_t k = std::min(n, 16 - used);
            std::memcpy(buf + used, m, k);
            used += k;
            m += k;
            n -= k;
            if (used == 16) {
                block(buf, 1u << 24);
                used = 0;
            }
        }
    }
    void finish(uint8_t tag[16]) {
        const uint32_t mask = 0x3ffffff;
        if (used > 0) {   // (the last, short block: a 1 after it, then zeros)
            buf[used] = 1;
            for (size_t i = used + 1; i < 16; ++i) buf[i] = 0;
            block(buf, 0);
        }
        uint32_t c = h[1] >> 26; h[1] &= mask;
        h[2] += c; c = h[2] >> 26; h[2] &= mask;
        h[3] += c; c = h[3] >> 26; h[3] &= mask;
        h[4] += c; c = h[4] >> 26; h[4] &= mask;
        h[0] += c * 5; c = h[0] >> 26; h[0] &= mask;
        h[1] += c;
        uint32_t g[5];   // h - p: used when it doesn't go negative
        g[0] = h[0] + 5; c = g[0] >> 26; g[0] &= mask;
        g[1] = h[1] + c; c = g[1] >> 26; g[1] &= mask;
        g[2] = h[2] + c; c = g[2] >> 26; g[2] &= mask;
        g[3] = h[3] + c; c = g[3] >> 26; g[3] &= mask;
        g[4] = h[4] + c - (1u << 26);
        uint32_t keep = (g[4] >> 31) - 1;
        for (int i = 0; i < 5; ++i) h[i] = (h[i] & ~keep) | (g[i] & keep);
        uint32_t w0 = h[0] | h[1] << 26;
        uint32_t w1 = h[1] >> 6 | h[2] << 20;
        uint32_t w2 = h[2] >> 12 | h[3] << 14;
        uint32_t w3 = h[3] >> 18 | h[4] << 8;
        uint64_t f = (uint64_t)w0 + pad[0];
        store32(tag, (uint32_t)f);
        f = (uint64_t)w1 + pad[1] + (f >> 32);
        store32(tag + 4, (uint32_t)f);
        f = (uint64_t)w2 + pad[2] + (f >> 32);
        store32(tag + 8, (uint32_t)f);
        f = (uint64_t)w3 + pad[3] + (f >> 32);
        store32(tag + 12, (uint32_t)f);
    }
};

// The AEAD's tag: Poly1305 over the aad and the ciphertext (each padded to 16) and their lengths
void aeadTag(const Key& key, const uint8_t nonce[12], const void* aad, size_t aadSize, const uint8_t* cipher, size_t size,
             uint8_t tag[16]) {
    uint8_t polyKey[64] = {};
    chacha20(key, 0, nonce, polyKey, sizeof(polyKey));
    Poly1305 p(polyKey);
    static const uint8_t kZeros[16] = {};
    p.update(static_cast<const uint8_t*>(aad), aadSize);
    p.update(kZeros, (16 - aadSize % 16) % 16);
    p.update(cipher, size);
    p.update(kZeros, (16 - size % 16) % 16);
    uint8_t lengths[16];
    store64(lengths, aadSize);
    store64(lengths + 8, size);
    p.update(lengths, 16);
    p.finish(tag);
}

}  // namespace

void poly1305(uint8_t tag[16], const void* message, size_t size, const uint8_t key[32]) {
    Poly1305 p(key);
    p.update(static_cast<const uint8_t*>(message), size);
    p.finish(tag);
}

void seal(const Key& key, const uint8_t nonce[12], const void* aad, size_t aadSize, const void* plain, size_t size,
          uint8_t* out) {
    std::memmove(out, plain, size);
    chacha20(key, 1, nonce, out, size);
    aeadTag(key, nonce, aad, aadSize, out, size, out + size);
}

bool open(const Key& key, const uint8_t nonce[12], const void* aad, size_t aadSize, const uint8_t* sealed, size_t size,
          uint8_t* out) {
    if (size < 16) return false;
    size -= 16;
    uint8_t tag[16];
    aeadTag(key, nonce, aad, aadSize, sealed, size, tag);
    if (!equal(tag, sealed + size, 16)) return false;
    std::memmove(out, sealed, size);
    chacha20(key, 1, nonce, out, size);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------------------------
Key passwordKey(const std::string& password) {
    if (password.empty()) return Key{};
    return hash("vkworld session password\n" + password);
}

bool Channel::derive(const KeyPair& mine, const Key& theirs, bool initiator, const Key& psk) {
    reset();
    Key shared;
    if (!sharedSecret(mine, theirs, shared)) return false;
    const Key& first = initiator ? mine.publicKey : theirs;   // (both sides hash the keys in the same order)
    const Key& second = initiator ? theirs : mine.publicKey;
    Key root;
    Blake2s(32, psk.data(), psk.size()).update("VKN3 session").update(shared.data(), 32).update(first.data(), 32)
        .update(second.data(), 32).final(root.data());
    Key toResponder, toInitiator;
    Blake2s(32, root.data(), 32).update("initiator to responder").final(toResponder.data());
    Blake2s(32, root.data(), 32).update("responder to initiator").final(toInitiator.data());
    send_ = initiator ? toResponder : toInitiator;
    receive_ = initiator ? toInitiator : toResponder;
    ready_ = true;
    return true;
}

std::vector<uint8_t> Channel::seal(const uint8_t* prefix, size_t prefixSize, const uint8_t* plain, size_t size) {
    std::vector<uint8_t> out(prefixSize + kOverhead + size);
    std::memcpy(out.data(), prefix, prefixSize);
    uint64_t counter = ++sent_;
    store64(out.data() + prefixSize, counter);
    uint8_t nonce[12] = {};
    store64(nonce + 4, counter);
    crypto::seal(send_, nonce, out.data(), prefixSize + 8, plain, size, out.data() + prefixSize + 8);
    return out;
}

bool Channel::open(const uint8_t* packet, size_t size, size_t prefixSize, std::vector<uint8_t>& plain) {
    if (!ready_ || size < prefixSize + kOverhead) return false;
    uint64_t counter = 0;
    for (int i = 7; i >= 0; --i) counter = counter << 8 | packet[prefixSize + i];
    if (counter == 0) return false;
    if (counter <= highest_) {   // (an older one: only if it's in the window and not seen yet)
        uint64_t behind = highest_ - counter;
        if (behind >= 64 || (window_ >> behind & 1)) return false;
    }
    uint8_t nonce[12] = {};
    store64(nonce + 4, counter);
    size_t sealedSize = size - prefixSize - 8;
    plain.resize(sealedSize - 16);
    if (!crypto::open(receive_, nonce, packet, prefixSize + 8, packet + prefixSize + 8, sealedSize, plain.data())) return false;
    if (counter > highest_) {   // (only once it's known to be genuine)
        uint64_t ahead = counter - highest_;
        window_ = ahead >= 64 ? 0 : window_ << ahead;
        window_ |= 1;
        highest_ = counter;
    } else {
        window_ |= (uint64_t)1 << (highest_ - counter);
    }
    return true;
}

}  // namespace crypto
