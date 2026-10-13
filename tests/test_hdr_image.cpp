// core/hdr_image: Radiance .hdr round trips (HDR screenshots) and half floats (render targets read back)
#include "test.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/hdr_image.h"

TEST(radiance_hdr_round_trip) {
    const uint32_t w = 5, h = 3;
    std::vector<float> rgb(w * h * 3);
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = std::pow(10.f, (float)(i % 9) - 3.f) * (1.f + 0.1f * (float)(i % 3));
    rgb[0] = rgb[1] = rgb[2] = 0.f;                                // black
    rgb[3] = -1.f;                                                 // (negative: none)
    rgb[6] = std::numeric_limits<float>::quiet_NaN();              // (not a number: black)
    std::string path = test::tempPath("shot.hdr").u8string();
    REQUIRE(writeRadianceHdr(path, w, h, rgb.data()));
    uint32_t rw = 0, rh = 0;
    std::vector<float> back;
    REQUIRE(readRadianceHdr(path, rw, rh, back));
    REQUIRE(rw == w && rh == h && back.size() == rgb.size());
    CHECK(back[0] == 0.f && back[1] == 0.f && back[2] == 0.f);
    CHECK(back[3] >= 0.f && back[3] <= rgb[5] * 0.01f);   // (beside brighter channels: half a step, about 0)
    CHECK(back[6] == 0.f && back[7] == 0.f);
    for (size_t p = 3; p < w * h; ++p) {   // each channel within RGBE's precision of its pixel's brightest
        float m = std::max(rgb[p * 3], std::max(rgb[p * 3 + 1], rgb[p * 3 + 2]));
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(back[p * 3 + (size_t)c] - rgb[p * 3 + (size_t)c]) <= m * 0.01f);
    }
    CHECK(!readRadianceHdr(test::tempPath("missing.hdr").u8string(), rw, rh, back));
}

TEST(half_floats) {
    CHECK(halfToFloat(0x3C00) == 1.f);
    CHECK(halfToFloat(0xC000) == -2.f);
    CHECK(halfToFloat(0x7BFF) == 65504.f);
    CHECK(halfToFloat(0x3555) > 0.333f && halfToFloat(0x3555) < 0.3334f);
    CHECK(halfToFloat(0x0001) == std::ldexp(1.f, -24));   // the smallest subnormal
    CHECK(halfToFloat(0x0200) == std::ldexp(1.f, -15));
    CHECK(std::isinf(halfToFloat(0x7C00)));
    CHECK(std::isnan(halfToFloat(0x7E00)));
    CHECK(halfToFloat(0x0000) == 0.f && std::signbit(halfToFloat(0x8000)));
}
