// render/texture_streaming: which mip levels textures keep on the GPU, under a budget, and the downsampling
#include "test.h"

#include <cmath>

#include "render/texture_streaming.h"

using streaming::TextureState;

TEST(streaming_levels_and_bytes) {
    CHECK(streaming::mipCount(1024, 512) == 11);
    CHECK(streaming::mipCount(1, 1) == 1);
    CHECK(streaming::chainBytes(4, 4, 0, 3) == (16 + 4 + 1) * 4);
    CHECK(streaming::chainBytes(4, 4, 1, 3) == (4 + 1) * 4);
    CHECK(streaming::levelForSize(2048, 1024, 256) == 3);
    CHECK(streaming::levelForSize(128, 128, 256) == 0);
}

TEST(streaming_plan) {
    streaming::Settings s;
    s.startSize = 256;
    s.forgetAfter = 3.0;
    auto tex = [](uint32_t size, uint32_t wanted, double when, bool pinned = false) {
        TextureState t;
        t.width = t.height = size;
        t.mips = streaming::mipCount(size, size);
        t.wanted = wanted;
        t.lastWanted = when;
        t.pinned = pinned;
        return t;
    };
    // What's wanted lately, the start level otherwise; pinned: whole
    std::vector<TextureState> ts = {tex(2048, 0, 9.5), tex(2048, 1, 2.0), tex(1024, 99, -1e9), tex(512, 5, 9.9, true),
                                    tex(64, 0, 9.0)};
    std::vector<uint32_t> p = streaming::plan(ts, s, 10.0);
    CHECK(p[0] == 0);   // asked for half a second ago
    CHECK(p[1] == 3);   // eight seconds ago: forgotten, back to 256
    CHECK(p[2] == 2);   // never: 256
    CHECK(p[3] == 0);   // pinned
    CHECK(p[4] == 0);   // small: whole anyway
    // Over budget: the one asked for longest ago gives up levels first, never past the start level
    std::vector<TextureState> two = {tex(2048, 0, 9.0), tex(2048, 0, 9.9)};
    s.budget = streaming::chainBytes(2048, 2048, 0, 12) + streaming::chainBytes(2048, 2048, 1, 12);
    p = streaming::plan(two, s, 10.0);
    CHECK(p[0] == 1 && p[1] == 0);
    s.budget = 1;   // (impossible: as coarse as allowed)
    p = streaming::plan(two, s, 10.0);
    CHECK(p[0] == 3 && p[1] == 3);
}

TEST(streaming_downsample) {
    // 4 x 2: left half black, right half white -> level 1 is 2 x 1: black, white; level 2 is 1 x 1, a mid grey
    std::vector<uint8_t> img(4 * 2 * 4, 255);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            for (int c = 0; c < 3; ++c) img[(size_t)(y * 4 + x) * 4 + c] = 0;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> l1 = streaming::downsample(img.data(), 4, 2, 1, w, h);
    REQUIRE(w == 2 && h == 1);
    CHECK(l1[0] == 0 && l1[4] == 255 && l1[7] == 255);
    std::vector<uint8_t> l2 = streaming::downsample(img.data(), 4, 2, 2, w, h);
    REQUIRE(w == 1 && h == 1);
    CHECK(l2[0] > 180 && l2[0] < 195);   // (half the light: sRGB ~188, not 128)
}

// A normal map's levels (data): its bytes averaged as numbers. Two normals leaning 45 degrees apart along x average to
// a shorter one pointing straight out (what specular anti-aliasing reads as roughness); as colours, x would be pulled
// towards the brighter byte and the normal tilted.
TEST(streaming_downsample_data) {
    std::vector<uint8_t> img = {218, 128, 218, 255, 37, 128, 218, 255};   // (+0.71, 0, 0.71) and (-0.71, 0, 0.71)
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> data = streaming::downsample(img.data(), 2, 1, 1, w, h, true);
    REQUIRE(w == 1 && h == 1);
    CHECK(data[0] >= 127 && data[0] <= 128);
    CHECK(data[1] == 128 && data[2] == 218 && data[3] == 255);
    float x = data[0] / 255.f * 2 - 1, z = data[2] / 255.f * 2 - 1;
    CHECK(std::sqrt(x * x + z * z) < 0.75f);   // (shorter: the two disagreed)
    std::vector<uint8_t> colour = streaming::downsample(img.data(), 2, 1, 1, w, h);
    CHECK(colour[0] > 150);   // (as colours: tilted towards +x)
}
