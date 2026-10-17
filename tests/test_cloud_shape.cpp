// Volumetric clouds' shape (render/cloud_shape.h): flat bases, domes as tall as the cover is dense
#include "test.h"

#include "render/cloud_shape.h"

TEST(cloud_shape_domes) {
    CHECK(clouds::density(0.f, 0.3f) == 0.f);   // (no cover: no cloud)
    CHECK(clouds::density(1.f, 0.f) == 0.f && clouds::density(1.f, 1.f) == 0.f);   // (outside the slab)
    // Dense cover: cloud from just above the base most of the way up
    CHECK(clouds::density(1.f, 0.1f) > 0.9f);
    CHECK(clouds::density(1.f, 0.7f) > 0.5f);
    // Thin cover: a low, flat cloud, gone further up
    CHECK(clouds::density(0.45f, 0.1f) > 0.5f);
    CHECK(clouds::density(0.45f, 0.6f) == 0.f);
    // The base is flat: the same just above it whatever the cover's edge
    CHECK(clouds::density(0.6f, 0.05f) > 0.99f && clouds::density(0.9f, 0.05f) > 0.99f);
    // Taller where denser: the height where it ends grows with the cover
    auto topOf = [](float cover) { float h = 0.05f;   // (from above the base's ramp)
        while (h < 1.f && clouds::density(cover, h + 0.01f) > 0.5f) h += 0.01f; return h; };
    CHECK(topOf(0.5f) < topOf(0.8f) && topOf(0.8f) < topOf(1.f));
}
