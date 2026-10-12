// render/display: the swapchain format for SDR or HDR, and the HDR encodings (the composite shader's reference)
#include "test.h"

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
