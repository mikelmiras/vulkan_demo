// Contact shadows from lamps (renderer.cpp's frame data, ssr_trace.frag): which lamps get them, and their four
// shadows packed into two floats of the half-size traces' distance image (three 8-bit values exactly in one, the
// fourth in the other). Plain functions, tested; the shaders repeat the packing.
#pragma once

#include <array>
#include <vector>

namespace contactlamps {

constexpr int kMax = 4;

// The first `kMax` lamps (the lights come nearest first) that cast shadows: their indices, -1 for the rest
std::array<int, kMax> choose(const std::vector<bool>& shadowed);

// Three contact shadows (0..1) into one float, exactly, and back
float pack3(float a, float b, float c);
void unpack3(float packed, float& a, float& b, float& c);

}  // namespace contactlamps
