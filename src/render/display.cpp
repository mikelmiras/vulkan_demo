#include "render/display.h"

#include <algorithm>
#include <cmath>

namespace display {

namespace {

bool isHdr10(const VkSurfaceFormatKHR& f) {
    return f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
           (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
}

bool isScRgb(const VkSurfaceFormatKHR& f) {
    return f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && f.format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// PQ's constants (SMPTE ST 2084)
constexpr float kM1 = 2610.f / 16384.f, kM2 = 2523.f / 4096.f * 128.f, kC1 = 3424.f / 4096.f,
                kC2 = 2413.f / 4096.f * 32.f, kC3 = 2392.f / 4096.f * 32.f;

}  // namespace

Choice choose(const std::vector<VkSurfaceFormatKHR>& available, bool wantHdr) {
    Choice c;
    if (available.empty()) return c;
    if (wantHdr) {
        for (const auto& f : available)
            if (isHdr10(f)) return {f, Output::Hdr10};
        for (const auto& f : available)
            if (isScRgb(f)) return {f, Output::ScRgb};
    }
    c.format = available[0];
    for (const auto& f : available)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            c.format = f;
            break;
        }
    return c;
}

bool hdrAvailable(const std::vector<VkSurfaceFormatKHR>& available) { return choose(available, true).output != Output::Sdr; }

float pqEncode(float nits) {
    float y = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), kM1);
    return std::pow((kC1 + kC2 * y) / (1.f + kC3 * y), kM2);
}

float pqDecode(float value) {
    float e = std::pow(std::clamp(value, 0.f, 1.f), 1.f / kM2);
    return 10000.f * std::pow(std::max(e - kC1, 0.f) / (kC2 - kC3 * e), 1.f / kM1);
}

Vec3 rec709To2020(Vec3 c) {
    return {0.6274f * c.x + 0.3293f * c.y + 0.0433f * c.z, 0.0691f * c.x + 0.9195f * c.y + 0.0114f * c.z,
            0.0164f * c.x + 0.0880f * c.y + 0.8956f * c.z};
}

}  // namespace display
