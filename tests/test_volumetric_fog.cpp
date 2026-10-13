// render/volumetric_fog: the fog volume's slices, its phase function and the integration its shaders do
#include "test.h"

#include <cmath>

#include "render/volumetric_fog.h"

namespace {
const double kPi = 3.14159265358979323846;
}

TEST(fog_slices) {
    fog::Range r{1.f, 256.f};
    CHECK_NEAR(fog::sliceDepth(r, 0.f), 1.f, 1e-4f);
    CHECK_NEAR(fog::sliceDepth(r, (float)fog::kCellsZ), 256.f, 1e-2f);
    CHECK_NEAR(fog::sliceDepth(r, 32.f), 16.f, 1e-3f);   // (logarithmic: halfway in slices, the geometric middle)
    // A distance maps to its slice's texel: the end of slice k is texel k's centre
    for (int k : {3, 20, 50}) CHECK_NEAR(fog::volumeZ(r, fog::sliceDepth(r, (float)(k + 1))), (k + 0.5f) / 64.f, 1e-4f);
    CHECK(fog::volumeZ(r, 0.01f) == 0.f && fog::volumeZ(r, 1e6f) == 1.f);
}

TEST(fog_phase) {
    Vec3 view{0, 0, -1};
    CHECK_NEAR(fog::phase({1, 0, 0}, view, 0.f), 1.f, 1e-5f);   // every way alike
    // Forward scattering: light coming at the camera (travelling against the view) turns towards it most
    CHECK(fog::phase({0, 0, 1}, view, 0.6f) > 5.f);
    CHECK(fog::phase({0, 0, -1}, view, 0.6f) < 0.2f);
    // Energy kept: averaged over every direction it's 1 (times 4 pi)
    double sum = 0;
    const int n = 200;
    for (int i = 0; i < n; ++i) {   // (over the angle to the view; the sphere's band at each)
        double t = (i + 0.5) / n * kPi;
        sum += fog::phase({(float)std::sin(t), 0.f, (float)std::cos(t)}, view, 0.6f) * std::sin(t) * (kPi / n) * 0.5;
    }
    CHECK_NEAR(sum, 1.0, 0.01);
}

TEST(fog_integration) {
    // Even fog lit evenly: the light gathered is that light times what the fog hides (1 - what gets through), and
    // what gets through is exp(-density x distance)
    fog::Range r{0.5f, 100.f};
    const float density = 0.05f;
    const Vec3 light{2.f, 1.f, 0.5f};
    std::vector<fog::Cell> column(fog::kCellsZ, {light * density, density});
    std::vector<fog::Gathered> g = fog::integrate(r, 1.f, column);
    REQUIRE(g.size() == (size_t)fog::kCellsZ);
    float far = fog::sliceDepth(r, (float)fog::kCellsZ);
    CHECK_NEAR(g.back().through, std::exp(-density * far), 1e-4f);
    CHECK_NEAR(g.back().light.x, light.x * (1.f - g.back().through), 1e-3f);
    CHECK_NEAR(g.back().light.z, light.z * (1.f - g.back().through), 1e-3f);
    for (size_t k = 1; k < g.size(); ++k) CHECK(g[k].through <= g[k - 1].through);
    // A slanted ray (twice the metres per metre along the view) loses twice as fast
    std::vector<fog::Gathered> s = fog::integrate(r, 2.f, column);
    CHECK_NEAR(s.back().through, std::exp(-density * far * 2.f), 1e-4f);
    // No fog: everything through, nothing added
    std::vector<fog::Gathered> clear = fog::integrate(r, 1.f, std::vector<fog::Cell>(fog::kCellsZ));
    CHECK_NEAR(clear.back().through, 1.f, 1e-4f);
    CHECK(clear.back().light.x == 0.f);
}
