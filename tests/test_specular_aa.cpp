// Specular anti-aliasing (render/specular_aa.h): highlights widened by the normals' spread within a pixel
#include "test.h"

#include <cmath>

#include "render/specular_aa.h"

TEST(specular_aa_normal_map_variance) {
    CHECK_NEAR(specaa::normalMapVariance(1.f, 1.f), 0.0, 1e-6);   // (flat, or a mip of alike normals)
    CHECK(specaa::normalMapVariance(0.9f, 1.f) > specaa::normalMapVariance(0.97f, 1.f));
    // Two normals 60 degrees apart, averaged (a mip of a bumpy map): length cos(30)
    const float len = std::cos(30.f * 3.14159265f / 180.f);
    CHECK_NEAR(specaa::normalMapVariance(len, 1.f), (1.0 - len) / len, 1e-6);
    CHECK_NEAR(specaa::normalMapVariance(len, 0.5f), (1.0 - len) / len * 0.25, 1e-6);   // (a fainter map)
    CHECK(std::isfinite(specaa::normalMapVariance(0.f, 1.f)));
}

TEST(specular_aa_widened_roughness) {
    CHECK_NEAR(specaa::widenedRoughness(0.3f, 0.f, 0.f), 0.3, 1e-6);   // (nothing varies: as it was)
    CHECK(specaa::widenedRoughness(0.1f, 0.01f, 0.f) > 0.1f);
    CHECK(specaa::widenedRoughness(0.1f, 0.f, 0.05f) > 0.1f);
    // Limited: a mirror with wildly varying normals stays glossy-ish (alpha^2 at most 0.36 from the two)
    CHECK(specaa::widenedRoughness(0.02f, 10.f, 10.f) < 0.8f);
    // Rough stays rough
    CHECK(specaa::widenedRoughness(0.9f, 0.05f, 0.05f) >= 0.9f);
}
