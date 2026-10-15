// render/display: the swapchain format for SDR or HDR, and the HDR encodings (the composite shader's reference)
#include "test.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include "render/display.h"

TEST(display_choose_format) {
    std::vector<VkSurfaceFormatKHR> sdrOnly = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                               {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    display::Choice c = display::choose(sdrOnly, true);   // (HDR asked for, not there: the UNORM one)
    CHECK(c.output == display::Output::Sdr && c.format.format == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(!display::hdrAvailable(sdrOnly));
    std::vector<VkSurfaceFormatKHR> all = sdrOnly;
    all.push_back({VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT});
    c = display::choose(all, true);
    CHECK(c.output == display::Output::ScRgb);
    all.push_back({VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT});
    c = display::choose(all, true);   // (HDR10 first)
    CHECK(c.output == display::Output::Hdr10 && c.format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    c = display::choose(all, false);
    CHECK(c.output == display::Output::Sdr && c.format.format == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(display::hdrAvailable(all));
}

TEST(display_pq_and_gamut) {
    CHECK_NEAR(display::pqEncode(0.f), 0.0, 1e-3);
    CHECK_NEAR(display::pqEncode(10000.f), 1.0, 1e-5);
    CHECK_NEAR(display::pqEncode(100.f), 0.508, 0.002);   // (the usual table value)
    CHECK_NEAR(display::pqEncode(1000.f), 0.752, 0.002);
    for (float nits : {0.5f, 80.f, 203.f, 4000.f}) CHECK_NEAR(display::pqDecode(display::pqEncode(nits)), nits, nits * 1e-3);
    Vec3 white = display::rec709To2020({1.f, 1.f, 1.f});   // (white stays white)
    CHECK_NEAR(white.x, 1.0, 1e-3);
    CHECK_NEAR(white.y, 1.0, 1e-3);
    CHECK_NEAR(white.z, 1.0, 1e-3);
    Vec3 red = display::rec709To2020({1.f, 0.f, 0.f});   // (709's red is inside 2020's: less saturated there)
    CHECK(red.x < 1.f && red.y > 0.f && red.z > 0.f);
}

// The display's light levels from its EDID: the CTA-861 extension's HDR static metadata block
TEST(display_edid_luminance) {
    std::vector<uint8_t> edid(256, 0);
    uint8_t* cta = edid.data() + 128;
    cta[0] = 0x02;   // a CTA-861 extension
    cta[1] = 3;
    const uint8_t blocks[] = {0x42, 0x10, 0x04,                      // video data block (two VICs)
                              0xE6, 0x06, 0x05, 0x01, 96, 64, 51};   // HDR static metadata: max, average, min
    std::copy(std::begin(blocks), std::end(blocks), cta + 4);
    cta[2] = (uint8_t)(4 + sizeof(blocks));   // (detailed timings start after the data blocks)
    display::Luminance l;
    REQUIRE(display::parseEdidLuminance(edid.data(), edid.size(), l));
    CHECK_NEAR(l.peak, 400.f, 0.01f);           // 50 * 2^(96 / 32)
    CHECK_NEAR(l.frameAverage, 200.f, 0.01f);   // 50 * 2^(64 / 32)
    CHECK_NEAR(l.min, 0.16f, 0.001f);           // 400 * (51 / 255)^2 / 100
    // Without the luminance bytes (an HDR block with EOTFs only): found, levels unknown
    cta[7] = 0xE3;
    cta[2] = 4 + 3 + 4;
    REQUIRE(display::parseEdidLuminance(edid.data(), edid.size(), l));
    CHECK(l.peak == 0.f);
    // No CTA extension, or only the base block: nothing
    cta[0] = 0x10;
    CHECK(!display::parseEdidLuminance(edid.data(), edid.size(), l));
    CHECK(!display::parseEdidLuminance(edid.data(), 128, l));
}

TEST(display_hdr_metadata) {
    display::Luminance screen;
    screen.peak = 600.f;
    screen.frameAverage = 350.f;
    screen.min = 0.05f;
    display::Metadata m = display::metadataFor(display::Output::Hdr10, 600.f, screen);
    CHECK_NEAR(m.red[0], 0.708f, 1e-4f);   // Rec.2020
    CHECK_NEAR(m.white[1], 0.329f, 1e-4f);
    CHECK(m.maxLuminance == 600.f && m.maxContentLight == 600.f);
    CHECK(m.maxFrameAverageLight == 350.f);
    CHECK(m.minLuminance == 0.05f);
    display::Metadata s = display::metadataFor(display::Output::ScRgb, 1000.f, {});
    CHECK_NEAR(s.green[1], 0.6f, 1e-4f);   // Rec.709
    CHECK(s.maxFrameAverageLight == 500.f);   // (unknown: half the peak)
}

// X11's UI scale: Xft.dpi in the X resources (KDE, GNOME, xrdb), else GDK_SCALE, else 1
TEST(display_x_resources_scale) {
    CHECK_NEAR(display::xResourcesScale("Xcursor.size:\t24\nXft.dpi:\t192\nXft.antialias:\t1\n", nullptr), 2.0, 1e-6);
    CHECK_NEAR(display::xResourcesScale("Xft.dpi: 144\n", "3"), 1.5, 1e-6);   // (Xft.dpi first)
    CHECK_NEAR(display::xResourcesScale("Xcursor.size:\t24\n", "2"), 2.0, 1e-6);
    CHECK_NEAR(display::xResourcesScale("", nullptr), 1.0, 1e-6);
    CHECK_NEAR(display::xResourcesScale("Xft.dpi:\tnonsense\n", "abc"), 1.0, 1e-6);
}
