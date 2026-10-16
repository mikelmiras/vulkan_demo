// The GPU benchmark's sums (render/gpu_benchmark.h) and the quality a measured frame calls for
#include "test.h"

#include "render/gpu_benchmark.h"

TEST(gpu_benchmark_medians) {
    GpuBenchmark b;
    b.add({{"Shadows", 2.f}, {"Scene", 10.f}}, 5.f);
    b.add({{"Shadows", 4.f}, {"Scene", 12.f}, {"Bloom", 1.f}}, 7.f);   // (a pass seen from the second frame on)
    b.add({{"Shadows", 3.f}, {"Scene", 30.f}}, 6.f);                  // (and missing from the third)
    CHECK(b.frames() == 3);
    CHECK_NEAR(b.medianTotal(), 17.0, 1e-5);   // (12, 17, 33)
    CHECK_NEAR(b.medianCpu(), 6.0, 1e-5);
    auto m = b.medians();
    REQUIRE(m.size() == 3);
    CHECK(m[0].first == "Shadows" && m[1].first == "Scene" && m[2].first == "Bloom");
    CHECK_NEAR(m[0].second, 3.0, 1e-5);
    CHECK_NEAR(m[1].second, 12.0, 1e-5);
    CHECK_NEAR(m[2].second, 0.0, 1e-5);   // (0, 1, 0)
    std::string j = b.json("NVIDIA \"Test\"", "city", "high");
    CHECK(j.find("\"gpu\": \"NVIDIA \\\"Test\\\"\"") != std::string::npos);
    CHECK(j.find("\"gpu_ms\": 17.000") != std::string::npos);
    CHECK(j.find("\"Scene\": 12.000") != std::string::npos);
}

TEST(gpu_benchmark_chooses_quality) {
    CHECK(chooseQuality(10.f, 16.7f) == 2);   // (high fits)
    CHECK(chooseQuality(22.f, 16.7f) == 1);   // (medium's two thirds fit)
    CHECK(chooseQuality(40.f, 16.7f) == 0);
    CHECK(chooseQuality(0.f, 16.7f) == 2);    // (nothing measured)
}
