#include "render/display.h"

#include <cstdlib>

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

float xResourcesScale(const std::string& resources, const char* gdkScale) {
    size_t at = resources.find("Xft.dpi:");
    if (at != std::string::npos) {
        float dpi = (float)std::atof(resources.c_str() + at + 8);   // (atof skips the tab)
        if (dpi >= 48.f && dpi <= 960.f) return dpi / 96.f;
    }
    if (gdkScale && *gdkScale) {
        int s = std::atoi(gdkScale);
        if (s >= 1 && s <= 8) return (float)s;
    }
    return 1.f;
}

bool parseEdidLuminance(const uint8_t* edid, size_t size, Luminance& out) {
    // 128-byte blocks: the base one, then extensions; a CTA-861 one (tag 2) has data blocks from byte 4 up to the
    // offset in byte 2, each a header (tag << 5 | length) and its bytes
    for (size_t block = 128; block + 128 <= size; block += 128) {
        const uint8_t* b = edid + block;
        if (b[0] != 0x02) continue;
        size_t end = std::min<size_t>(b[2], 127);
        for (size_t at = 4; at < end;) {
            int tag = b[at] >> 5, length = b[at] & 31;
            if (at + 1 + (size_t)length > end) break;
            // "Use extended tag" (7), extended tag 6: HDR static metadata (EOTFs, descriptors, then up to three
            // luminance code values: max, max frame average, min)
            if (tag == 7 && length >= 3 && b[at + 1] == 6) {
                Luminance l;
                if (length >= 4 && b[at + 4]) l.peak = 50.f * std::pow(2.f, (float)b[at + 4] / 32.f);
                if (length >= 5 && b[at + 5]) l.frameAverage = 50.f * std::pow(2.f, (float)b[at + 5] / 32.f);
                if (length >= 6 && l.peak > 0.f) l.min = l.peak * std::pow((float)b[at + 6] / 255.f, 2.f) / 100.f;
                out = l;
                return true;
            }
            at += 1 + (size_t)length;
        }
    }
    return false;
}

Metadata metadataFor(Output output, float peak, const Luminance& display) {
    Metadata m{};
    if (output == Output::Hdr10) {   // Rec.2020
        m.red[0] = 0.708f, m.red[1] = 0.292f, m.green[0] = 0.170f, m.green[1] = 0.797f, m.blue[0] = 0.131f, m.blue[1] = 0.046f;
    } else {                         // Rec.709 (scRGB)
        m.red[0] = 0.640f, m.red[1] = 0.330f, m.green[0] = 0.300f, m.green[1] = 0.600f, m.blue[0] = 0.150f, m.blue[1] = 0.060f;
    }
    m.white[0] = 0.3127f, m.white[1] = 0.3290f;   // D65
    m.maxLuminance = peak;
    m.minLuminance = display.min > 0.f ? display.min : 0.005f;
    m.maxContentLight = peak;
    m.maxFrameAverageLight = display.frameAverage > 0.f ? std::min(display.frameAverage, peak) : peak * 0.5f;
    return m;
}

}  // namespace display
