// net/crypto: X25519, ChaCha20-Poly1305 and BLAKE2s against their RFCs' (and Python cryptography's) vectors, and
// channels: both sides agree, tampering, replays and the wrong password are refused
#include "test.h"

#include <cstring>

#include "net/crypto.h"

namespace {

std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) out.push_back((uint8_t)std::stoi(std::string(s + i, 2), nullptr, 16));
    return out;
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

crypto::Key key(const char* s) {
    crypto::Key k{};
    std::vector<uint8_t> b = hex(s);
    std::memcpy(k.data(), b.data(), std::min(b.size(), k.size()));
    return k;
}

}  // namespace

TEST(crypto_x25519) {
    // RFC 7748, 6.1
    crypto::KeyPair alice, bob;
    alice.secret = key("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    bob.secret = key("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const uint8_t base[32] = {9};
    crypto::x25519(alice.publicKey.data(), alice.secret.data(), base);
    crypto::x25519(bob.publicKey.data(), bob.secret.data(), base);
    CHECK(toHex(alice.publicKey.data(), 32) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(toHex(bob.publicKey.data(), 32) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    crypto::Key ab, ba;
    REQUIRE(crypto::sharedSecret(alice, bob.publicKey, ab));
    REQUIRE(crypto::sharedSecret(bob, alice.publicKey, ba));
    CHECK(toHex(ab.data(), 32) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(ab == ba);
    // Fresh pairs agree too; a point of small order (zero) gives no secret
    crypto::KeyPair x = crypto::KeyPair::generate(), y = crypto::KeyPair::generate();
    CHECK(x.secret != y.secret);
    crypto::Key xy, yx;
    REQUIRE(crypto::sharedSecret(x, y.publicKey, xy));
    REQUIRE(crypto::sharedSecret(y, x.publicKey, yx));
    CHECK(xy == yx);
    CHECK(!crypto::sharedSecret(x, crypto::Key{}, xy));
}

TEST(crypto_chacha20_poly1305) {
    // Poly1305: RFC 8439, 2.5.2
    std::vector<uint8_t> pk = hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    const char* msg = "Cryptographic Forum Research Group";
    uint8_t tag[16];
    crypto::poly1305(tag, msg, std::strlen(msg), pk.data());
    CHECK(toHex(tag, 16) == "a8061dc1305136c6c22b8baf0c0127a9");
    // The AEAD: RFC 8439, 2.8.2
    crypto::Key k = key("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    std::vector<uint8_t> nonce = hex("070000004041424344454647"), aad = hex("50515253c0c1c2c3c4c5c6c7");
    std::string plain = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                        "sunscreen would be it.";
    std::vector<uint8_t> sealed(plain.size() + 16);
    crypto::seal(k, nonce.data(), aad.data(), aad.size(), plain.data(), plain.size(), sealed.data());
    CHECK(toHex(sealed.data(), 16) == "d31a8d34648e60db7b86afbc53ef7ec2");
    CHECK(toHex(sealed.data() + plain.size(), 16) == "1ae10b594f09e26a7e902ecbd0600691");
    std::vector<uint8_t> back(plain.size());
    REQUIRE(crypto::open(k, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), back.data()));
    CHECK(std::string(back.begin(), back.end()) == plain);
    // Any change is refused: a ciphertext bit, the tag, the aad
    sealed[5] ^= 1;
    CHECK(!crypto::open(k, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), back.data()));
    sealed[5] ^= 1;
    sealed.back() ^= 0x80;
    CHECK(!crypto::open(k, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), back.data()));
    sealed.back() ^= 0x80;
    aad[0] ^= 1;
    CHECK(!crypto::open(k, nonce.data(), aad.data(), aad.size(), sealed.data(), sealed.size(), back.data()));
}

TEST(crypto_blake2s) {
    crypto::Key h = crypto::hash("abc");
    CHECK(toHex(h.data(), 32) == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");
    uint8_t data[200];
    for (int i = 0; i < 200; ++i) data[i] = (uint8_t)i;
    h = crypto::hash(data, 64);   // (exactly one block)
    CHECK(toHex(h.data(), 32) == "56f34e8b96557e90c1f24b52d0c89d51086acf1b00f634cf1dde9233b8eaaa3e");
    crypto::Key k;
    for (int i = 0; i < 32; ++i) k[i] = (uint8_t)i;
    h = crypto::mac(k, data, 200);
    CHECK(toHex(h.data(), 32) == "13c88480a5d00d6c8c7ad2110d76a82d9b70f4fa6696d4e5dd42a066dcaf9920");
    // In pieces, the same; a shorter output
    crypto::Blake2s b(32, k.data(), 32);
    b.update(data, 7).update(data + 7, 100).update(data + 107, 93);
    crypto::Key pieces;
    b.final(pieces.data());
    CHECK(pieces == h);
    uint8_t short16[16];
    crypto::Blake2s(16).update("abc").final(short16);
    CHECK(toHex(short16, 16) == "aa4938119b1dc7b87cbad0ffd200d0ae");
}

TEST(crypto_channels) {
    crypto::KeyPair client = crypto::KeyPair::generate(), server = crypto::KeyPair::generate();
    crypto::Key psk = crypto::passwordKey("hunter2");
    crypto::Channel c, s;
    REQUIRE(c.derive(client, server.publicKey, true, psk));
    REQUIRE(s.derive(server, client.publicKey, false, psk));
    const uint8_t prefix[5] = {'V', 'K', 'N', '3', 9};
    const char* hello = "hello server";
    std::vector<uint8_t> p1 = c.seal(prefix, 5, (const uint8_t*)hello, std::strlen(hello));
    CHECK(p1.size() == 5 + crypto::Channel::kOverhead + std::strlen(hello));
    CHECK(std::memcmp(p1.data(), prefix, 5) == 0);
    CHECK(std::string(p1.begin(), p1.end()).find("hello") == std::string::npos);   // (not in the clear)
    std::vector<uint8_t> out;
    REQUIRE(s.open(p1.data(), p1.size(), 5, out));
    CHECK(std::string(out.begin(), out.end()) == hello);
    CHECK(!s.open(p1.data(), p1.size(), 5, out));   // a replay
    CHECK(!c.open(p1.data(), p1.size(), 5, out));   // (the client's own: the other direction's key)
    // Out of order is fine within the window; the prefix is covered too
    std::vector<uint8_t> p2 = c.seal(prefix, 5, (const uint8_t*)"two", 3), p3 = c.seal(prefix, 5, (const uint8_t*)"three", 5);
    REQUIRE(s.open(p3.data(), p3.size(), 5, out));
    REQUIRE(s.open(p2.data(), p2.size(), 5, out));
    CHECK(std::string(out.begin(), out.end()) == "two");
    std::vector<uint8_t> p4 = c.seal(prefix, 5, (const uint8_t*)"four", 4);
    p4[4] = 1;
    CHECK(!s.open(p4.data(), p4.size(), 5, out));
    p4[4] = 9;
    REQUIRE(s.open(p4.data(), p4.size(), 5, out));
    // Too old (over 64 behind) is refused even if never seen
    std::vector<uint8_t> old = c.seal(prefix, 5, (const uint8_t*)"old", 3);
    for (int i = 0; i < 70; ++i) {
        std::vector<uint8_t> p = c.seal(prefix, 5, (const uint8_t*)"x", 1);
        REQUIRE(s.open(p.data(), p.size(), 5, out));
    }
    CHECK(!s.open(old.data(), old.size(), 5, out));
    // And back the other way
    std::vector<uint8_t> back = s.seal(prefix, 5, (const uint8_t*)"welcome", 7);
    REQUIRE(c.open(back.data(), back.size(), 5, out));
    // The wrong password: the keys differ, nothing opens
    crypto::Channel wrong;
    REQUIRE(wrong.derive(server, client.publicKey, false, crypto::passwordKey("hunter3")));
    std::vector<uint8_t> p5 = c.seal(prefix, 5, (const uint8_t*)"hi", 2);
    CHECK(!wrong.open(p5.data(), p5.size(), 5, out));
    CHECK(crypto::passwordKey("") == crypto::Key{});
}

TEST(crypto_sha512) {
    auto hexOf = [](const std::array<uint8_t, 64>& d) { return toHex(d.data(), d.size()); };
    CHECK(hexOf(crypto::sha512("abc", 3)) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    std::vector<uint8_t> data;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; i < 256; ++i) data.push_back((uint8_t)i);
    CHECK(hexOf(crypto::sha512(data.data(), data.size())) ==
          "f1721f49518ee462a3d81def26d81037cd474b4254b85ad7c8f1509594d0177bbb996ee9625813852bacac108c2a72c83a8587050fec1dcda64730d6470953e6");
    std::vector<uint8_t> zeros(112, 0);   // (111 bytes: padding fits one block; 112: it takes two)
    CHECK(hexOf(crypto::sha512(zeros.data(), 111)) ==
          "77ddd3a542e530fd047b8977c657ba6ce72f1492e360b2b2212cd264e75ec03882e4ff0525517ab4207d14c70c2259ba88d4d335ee0e7e20543d22102ab1788c");
    CHECK(hexOf(crypto::sha512(zeros.data(), 112)) ==
          "2be2e788c8a8adeaa9c89a7f78904cacea6e39297d75e0573a73c756234534d6627ab4156b48a6657b29ab8beb73334040ad39ead81446bb09c70704ec707952");
}

TEST(crypto_ed25519) {
    struct Vector {
        const char *seed, *message, *publicKey, *signature;
    } vectors[] = {   // RFC 8032's first two, and one of ours (checked with Python's cryptography)
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "r",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"0000000000000000000000000000000000000000000000000000000000000001", "manifest body",
         "4cb5abf6ad79fbf5abbccafcc269d85cd2651ed4b885b5869f241aedf0a5ba29",
         "67d8e949f4c28da1837b516f6a46104b3af919213290be690b06f44e7e74f34476f6e6eb06c3b8afb9d5ba7b2a1c1532f841928e1255cb0917aea31a9cf8df0e"},
    };
    for (const Vector& v : vectors) {
        uint8_t seed[32];
        REQUIRE(crypto::fromHex(v.seed, seed, 32));
        crypto::SigningKey key = crypto::SigningKey::fromSeed(seed);
        CHECK(toHex(key.publicKey.data(), 32) == v.publicKey);
        std::array<uint8_t, 64> sig = crypto::sign(key, v.message, std::strlen(v.message));
        CHECK(toHex(sig.data(), 64) == v.signature);
        CHECK(crypto::verify(key.publicKey.data(), v.message, std::strlen(v.message), sig.data()));
        std::string other = std::string(v.message) + "!";
        CHECK(!crypto::verify(key.publicKey.data(), other.data(), other.size(), sig.data()));   // another message
        sig[5] ^= 1;
        CHECK(!crypto::verify(key.publicKey.data(), v.message, std::strlen(v.message), sig.data()));   // a changed signature
    }
    crypto::SigningKey a = crypto::SigningKey::generate(), b = crypto::SigningKey::generate();
    std::array<uint8_t, 64> sig = crypto::sign(a, "hello", 5);
    CHECK(crypto::verify(a.publicKey.data(), "hello", 5, sig.data()));
    CHECK(!crypto::verify(b.publicKey.data(), "hello", 5, sig.data()));   // someone else's key
    CHECK(crypto::toHex("\x01\xab", 2) == "01ab");
    uint8_t two[2];
    CHECK(crypto::fromHex("01AB", two, 2) && two[0] == 1 && two[1] == 0xab);
    CHECK(!crypto::fromHex("01", two, 2) && !crypto::fromHex("zz00", two, 2));
}
