// Cryptography for multiplayer sessions, the relay and signed updates: small, dependency-free, constant-time where it
// matters.
//
//   - X25519 (RFC 7748): Diffie-Hellman key agreement (each side's key pair; both get the same shared secret)
//   - ChaCha20-Poly1305 (RFC 8439): authenticated encryption (a packet can be neither read nor changed in transit)
//   - BLAKE2s-256 (RFC 7693): hashing, keyed hashing (MACs) and deriving keys
//   - Ed25519 (RFC 8032, with SHA-512): signatures
//   - randomBytes: the operating system's secure random numbers
//
// Plain code with no engine dependencies (vkrelay is built with it).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crypto {

using Key = std::array<uint8_t, 32>;
using Tag = std::array<uint8_t, 16>;

void randomBytes(void* out, size_t size);
bool equal(const void* a, const void* b, size_t size);   // (compares every byte: no timing hints)

// X25519
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
struct KeyPair {
    Key secret{}, publicKey{};
    static KeyPair generate();
};
// The secret both sides of an exchange share (their secret, the other's public key); false for a degenerate
// public key (all zeros out: someone sent a point of small order)
bool sharedSecret(const KeyPair& mine, const Key& theirs, Key& out);

// BLAKE2s
class Blake2s {
public:
    explicit Blake2s(size_t outSize = 32, const void* key = nullptr, size_t keySize = 0);
    Blake2s& update(const void* data, size_t size);
    Blake2s& update(const std::string& s) { return update(s.data(), s.size()); }
    void final(uint8_t* out);

private:
    void compress(bool last);
    uint32_t h_[8];
    uint32_t t_[2] = {0, 0};
    uint8_t buf_[64];
    size_t used_ = 0, outSize_;
};
Key hash(const void* data, size_t size);
Key hash(const std::string& s);
Key mac(const Key& key, const void* data, size_t size);   // keyed BLAKE2s

// ChaCha20-Poly1305: `out` gets size + 16 bytes (the tag last); open() writes size - 16 and checks the tag
void seal(const Key& key, const uint8_t nonce[12], const void* aad, size_t aadSize, const void* plain, size_t size,
          uint8_t* out);
bool open(const Key& key, const uint8_t nonce[12], const void* aad, size_t aadSize, const uint8_t* sealed, size_t size,
          uint8_t* out);
// (the parts, exposed for their tests)
void chacha20(const Key& key, uint32_t counter, const uint8_t nonce[12], uint8_t* data, size_t size);   // xor in place
void poly1305(uint8_t tag[16], const void* message, size_t size, const uint8_t key[32]);

// SHA-512 (FIPS 180-4), for Ed25519
std::array<uint8_t, 64> sha512(const void* data, size_t size);

// Ed25519 signatures (RFC 8032): a key pair from a 32-byte seed (the secret), messages signed with it, checked with
// the public key alone (update manifests: app/updates.h)
struct SigningKey {
    std::array<uint8_t, 32> seed{}, publicKey{};
    static SigningKey fromSeed(const uint8_t seed[32]);
    static SigningKey generate();
};
std::array<uint8_t, 64> sign(const SigningKey& key, const void* message, size_t size);
bool verify(const uint8_t publicKey[32], const void* message, size_t size, const uint8_t signature[64]);

// Hex text of bytes, and back (false: not hex, or not `size` bytes)
std::string toHex(const void* data, size_t size);
bool fromHex(const std::string& text, uint8_t* out, size_t size);

// A password's key, mixed into a key exchange so only those who know it agree on the keys (zeros: no password)
Key passwordKey(const std::string& password);

// One side of an encrypted conversation after a key exchange: a key each way; packets numbered (the counter is the
// nonce), and one seen before, or over 64 behind the newest, refused (replays)
class Channel {
public:
    static constexpr size_t kOverhead = 8 + 16;   // the counter and the tag

    // initiator: the side whose key went first (a client); both sides must pass the same psk
    bool derive(const KeyPair& mine, const Key& theirs, bool initiator, const Key& psk);
    bool ready() const { return ready_; }
    void reset() { *this = Channel{}; }
    // prefix (sent as it is, but covered by the tag) + counter + ciphertext + tag
    std::vector<uint8_t> seal(const uint8_t* prefix, size_t prefixSize, const uint8_t* plain, size_t size);
    // A sealed packet's plaintext; false if it was changed, sealed with other keys, or is a replay
    bool open(const uint8_t* packet, size_t size, size_t prefixSize, std::vector<uint8_t>& plain);

private:
    Key send_{}, receive_{};
    uint64_t sent_ = 0, highest_ = 0, window_ = 0;   // (bit i of window_: highest_ - i was received)
    bool ready_ = false;
};

}  // namespace crypto
