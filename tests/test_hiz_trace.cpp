// The reflections' walk over the near pyramid (render/hiz_trace.h): the hits of a texel-by-texel march, in fewer
// samples over open space
#include "test.h"

#include <cstdint>

#include "render/hiz_trace.h"

TEST(hiz_trace_finds_the_march_hits) {
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.f; };
    int fewer = 0, rows = 0;
    for (int trial = 0; trial < 200; ++trial) {
        // A row of far background with a few near things (posts) and a slope
        std::vector<float> row(512);
        for (size_t i = 0; i < row.size(); ++i) row[i] = 200.f - (float)i * 0.05f;
        const int posts = 1 + (int)(rnd() * 4);
        for (int k = 0; k < posts; ++k) {
            int at = (int)(rnd() * 500.f), w = 1 + (int)(rnd() * 6.f);
            float z = 5.f + rnd() * 60.f;
            for (int i = at; i < at + w && i < (int)row.size(); ++i) row[(size_t)i] = z;
        }
        const auto pyramid = hiztrace::nearPyramid(row);
        const int from = (int)(rnd() * 40.f);
        const float z0 = 1.f + rnd() * 3.f, dz = 0.02f + rnd() * 0.3f;
        const hiztrace::Hit fine = hiztrace::march(row, from, z0, dz);
        const hiztrace::Hit fast = hiztrace::walk(pyramid, from, z0, dz, 4);
        CHECK(fast.at == fine.at);
        ++rows;
        fewer += fast.samples < fine.samples ? 1 : 0;
    }
    CHECK(fewer > rows * 3 / 4);   // (most rays: fewer samples)
}

TEST(hiz_trace_open_space) {
    // Nothing near anywhere: across 512 texels in a handful of 16-texel jumps, no hit
    std::vector<float> row(512, 1000.f);
    const auto pyramid = hiztrace::nearPyramid(row);
    CHECK(pyramid.size() == 10 && pyramid[4].size() == 32);
    const hiztrace::Hit h = hiztrace::walk(pyramid, 0, 1.f, 0.1f, 4);
    CHECK(h.at == -1);
    CHECK(h.samples < 40);
    CHECK(hiztrace::march(row, 0, 1.f, 0.1f).samples == 512);
}

// The GI rays' jumps: the march's hits, fewer samples where the pyramid's tiles are clear
TEST(hiz_trace_gi_jumps) {
    uint32_t seed = 777;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.f; };
    int fewer = 0;
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<float> row(256);
        for (size_t i = 0; i < row.size(); ++i) row[i] = 60.f - (float)i * 0.02f;
        int at = (int)(rnd() * 250.f), w = 1 + (int)(rnd() * 4.f);
        for (int i = at; i < at + w && i < (int)row.size(); ++i) row[(size_t)i] = 2.f + rnd() * 10.f;
        const auto pyramid = hiztrace::nearPyramid(row);
        const float z0 = 1.f + rnd(), dz = 0.01f + rnd() * 0.1f;
        const hiztrace::Hit fine = hiztrace::march(row, 0, z0, dz), quick = hiztrace::jumpMarch(pyramid, 0, z0, dz, 3);
        CHECK(quick.at == fine.at);
        fewer += quick.samples < fine.samples ? 1 : 0;
    }
    CHECK(fewer > 150);
}
