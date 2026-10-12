// The display: standard (SDR) or high dynamic range output, and the encodings the composite shader uses for it
// (composite.frag; the C++ versions here are its reference, for tests). HDR10: Rec.2020 primaries, PQ (SMPTE ST 2084)
// encoded absolute luminance, 10 bits; scRGB: linear Rec.709 in half floats, 1.0 = 80 nits.
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

#include "core/vecmath.h"

namespace display {

enum class Output { Sdr, Hdr10, ScRgb };

struct Choice {
    VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    Output output = Output::Sdr;
};

// What to render into from what the surface offers: an 8-bit UNORM sRGB-encoded format (the composite does the gamma
// itself, so the UI's colours stay right); with `wantHdr`, HDR10 if it's there, else scRGB, else that
Choice choose(const std::vector<VkSurfaceFormatKHR>& available, bool wantHdr);
bool hdrAvailable(const std::vector<VkSurfaceFormatKHR>& available);

float pqEncode(float nits);   // 0..10000 nits -> 0..1
float pqDecode(float value);
Vec3 rec709To2020(Vec3 linear);

}  // namespace display
