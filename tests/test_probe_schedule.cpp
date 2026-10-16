// Reflection probes' bakes over several frames (render/probe_schedule.h)
#include "test.h"

#include "render/probe_schedule.h"

TEST(probe_faces_done) {
    // Two frames in flight: the frame that drew the last face (10) is done once its slot comes round again and one
    // more (a frame that never submitted can't make it early)
    CHECK(!probes::facesDone(10, 10, 2));
    CHECK(!probes::facesDone(12, 10, 2));
    CHECK(probes::facesDone(13, 10, 2));
    CHECK(probes::facesDone(100, 10, 2));
    CHECK(probes::facesDone(3, 0, 2));   // (never drawn: nothing in flight)
}

TEST(probe_eviction) {
    CHECK(probes::evictionVictim({5, 2, 9, 1}, {false, false, false, false}) == 3);   // (the least recently used)
    CHECK(probes::evictionVictim({5, 2, 9, 1}, {false, false, false, true}) == 1);    // (not one in use now)
    CHECK(probes::evictionVictim({5, 2}, {true, true}) == -1);
    CHECK(probes::evictionVictim({}, {}) == -1);
}
