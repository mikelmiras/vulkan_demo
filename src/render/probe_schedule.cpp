#include "render/probe_schedule.h"

namespace probes {

bool facesDone(uint64_t now, uint64_t lastDrawn, int inFlight) { return now > lastDrawn + (uint64_t)inFlight; }

int evictionVictim(const std::vector<uint64_t>& lastUsed, const std::vector<bool>& active) {
    int victim = -1;
    for (size_t i = 0; i < lastUsed.size(); ++i) {
        if (i < active.size() && active[i]) continue;
        if (victim < 0 || lastUsed[i] < lastUsed[(size_t)victim]) victim = (int)i;
    }
    return victim;
}

}  // namespace probes
