// Reflection probes' bakes over several frames (renderer_probes.cpp): when a bake's faces are safe to read (and its
// targets to free), and which baked probe makes room for a new one. Plain functions, tested.
#pragma once

#include <cstdint>
#include <vector>

namespace probes {

// Whether the frame that drew a bake's last face (`lastDrawn`, a frame count) is done on the GPU at frame `now`, with
// `inFlight` frames in flight: its fence has been waited for (one frame's margin for a frame that never submitted)
bool facesDone(uint64_t now, uint64_t lastDrawn, int inFlight);

// The baked probe a new one replaces when there's no room: the least recently used (`lastUsed`) of those not in use
// this frame (`active`); -1 if all are
int evictionVictim(const std::vector<uint64_t>& lastUsed, const std::vector<bool>& active);

}  // namespace probes
