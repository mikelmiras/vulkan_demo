// render/starburst: an iris's streaks, and its settings packed for the composite
#include "test.h"

#include "render/starburst.h"

TEST(starburst_lines_and_packing) {
    CHECK(starburst::lines(6) == 3);   // (six streaks: three lines through the spot)
    CHECK(starburst::lines(4) == 2);
    CHECK(starburst::lines(5) == 5);   // (an odd iris: ten streaks)
    CHECK(starburst::lines(7) == 7);
    for (int blades = 4; blades <= 9; ++blades)
        for (float strength : {0.f, 0.3f, 1.f}) {
            int b = 0;
            float s = -1.f;
            starburst::unpack(starburst::pack(blades, strength), b, s);
            CHECK(b == blades);
            CHECK_NEAR(s, strength, 1e-5);
        }
    int b = 0;
    float s = 0.f;
    starburst::unpack(starburst::pack(12, 2.f), b, s);   // (clamped)
    CHECK(b == 9 && s <= 1.f + 1e-5f);
}
