// The display: standard (SDR) or high dynamic range output, and the encodings the composite shader uses for it
// (composite.frag; the C++ versions here are its reference, for tests). HDR10: Rec.2020 primaries, PQ (SMPTE ST 2084)
// encoded absolute luminance, 10 bits; scRGB: linear Rec.709 in half floats, 1.0 = 80 nits.
#pragma once

#include <string>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
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

// What a display can show (nits; 0: unknown), as it reports it: the EDID's HDR static metadata on Linux, DXGI on
// Windows, the extended dynamic range headroom on macOS (platform::displayLuminance)
struct Luminance {
    float peak = 0.f;           // brightest, on a small part of the screen
    float frameAverage = 0.f;   // brightest over the whole screen
    float min = 0.f;
};
// The HDR static metadata data block in an EDID's CTA-861 extensions (false if there's none)
bool parseEdidLuminance(const uint8_t* edid, size_t size, Luminance& out);

// X11's UI scale: Xft.dpi from the root window's X resources ("Xft.dpi:\t192" -> 2), else GDK_SCALE (an integer:
// GNOME's), over 96 dpi; 1 if neither says
float xResourcesScale(const std::string& resources, const char* gdkScale);

// What the swapchain tells the display about the picture (VK_EXT_hdr_metadata): the primaries it's mastered in
// (Rec.2020 for HDR10, Rec.709 for scRGB), D65 white, and its light levels (the display's own: the tone mapper
// keeps highlights under its peak)
struct Metadata {
    float red[2], green[2], blue[2], white[2];   // CIE 1931 xy
    float maxLuminance, minLuminance;            // the mastering display's (nits)
    float maxContentLight, maxFrameAverageLight; // MaxCLL, MaxFALL
};
Metadata metadataFor(Output output, float peak, const Luminance& display);

float pqEncode(float nits);   // 0..10000 nits -> 0..1
float pqDecode(float value);
Vec3 rec709To2020(Vec3 linear);

}  // namespace display
