// Contact shadows from lamps (render/contact_lamps.h): which lamps, and their shadows packed exactly
#include "test.h"

#include "render/contact_lamps.h"

TEST(contact_lamps_chosen) {
    auto c = contactlamps::choose({false, true, true, false, true, true, true});
    CHECK(c[0] == 1 && c[1] == 2 && c[2] == 4 && c[3] == 5);   // (the nearest four that cast shadows)
    c = contactlamps::choose({false, true});
    CHECK(c[0] == 1 && c[1] == -1 && c[2] == -1 && c[3] == -1);
    c = contactlamps::choose({});
    CHECK(c[0] == -1);
}

TEST(contact_lamps_packed) {
    for (float a : {0.f, 0.25f, 1.f})
        for (float b : {0.f, 0.6f, 1.f})
            for (float c : {0.f, 0.1f, 1.f}) {
                float x, y, z;
                contactlamps::unpack3(contactlamps::pack3(a, b, c), x, y, z);
                CHECK_NEAR(x, a, 0.5 / 255.0);
                CHECK_NEAR(y, b, 0.5 / 255.0);
                CHECK_NEAR(z, c, 0.5 / 255.0);
            }
    CHECK(contactlamps::pack3(1.f, 1.f, 1.f) < 16777216.f);   // (whole in a float: 24 bits)
}
