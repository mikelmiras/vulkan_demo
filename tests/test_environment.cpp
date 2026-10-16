// render/environment: sky panoramas prefiltered for lighting
#include "test.h"

#include <chrono>

#include "render/environment.h"

namespace {

std::vector<float> sky(uint32_t w, uint32_t h, Vec3 (*f)(Vec3)) {
    std::vector<float> rgb((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            Vec3 c = f(environment::directionAt(((float)x + 0.5f) / (float)w, ((float)y + 0.5f) / (float)h));
            float* p = &rgb[((size_t)y * w + x) * 3];
            p[0] = c.x, p[1] = c.y, p[2] = c.z;
        }
    return rgb;
}

}  // namespace

TEST(environment_directions) {
    for (Vec3 d : {Vec3{0, 0, -1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, normalize(Vec3{0.3f, -0.5f, 0.8f})}) {
        float u, v;
        environment::uvOf(d, u, v);
        Vec3 back = environment::directionAt(u, v);
        CHECK_NEAR(dot(back, d), 1.f, 1e-4);
    }
    float u, v;
    environment::uvOf({0, 0, -1}, u, v);
    CHECK_NEAR(u, 0.5f, 1e-6);   // -Z in the middle
    CHECK_NEAR(v, 0.5f, 1e-6);
}

// A uniform sky: everything gets the same light; a sky lit from above: bright up, dark down
TEST(environment_irradiance) {
    Environment flat = environment::build("flat", 128, 64, sky(128, 64, [](Vec3) { return Vec3{1, 1, 1}; }));
    for (Vec3 n : {Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}, normalize(Vec3{1, 1, 1})})
        CHECK_NEAR(environment::irradiance(flat, n).x, 1.f, 0.03);
    REQUIRE(flat.radiance.size() == (size_t)environment::kRadianceLevels);
    CHECK(flat.radiance[1].width == 128 && flat.radiance[5].width == 8 && flat.radiance[5].height == 4);
    CHECK_NEAR(environment::sample(flat.radiance[3], {0, 0, -1}).y, 1.f, 0.01);

    Environment above = environment::build("above", 128, 64, sky(128, 64, [](Vec3 d) {
        return d.y > 0.f ? Vec3{2, 2, 2} : Vec3{0, 0, 0};
    }));
    CHECK_NEAR(environment::irradiance(above, {0, 1, 0}).x, 2.f, 0.15);
    CHECK_NEAR(environment::irradiance(above, {0, -1, 0}).x, 0.f, 0.15);
    CHECK_NEAR(environment::irradiance(above, {1, 0, 0}).x, 1.f, 0.1);
}

// A small bright spot: sharp at level 0, spread wider (and dimmer at its centre) the rougher the level
TEST(environment_prefiltered_reflections) {
    Vec3 (*spot)(Vec3) = [](Vec3 d) { return dot(d, Vec3{0, 0, -1}) > 0.995f ? Vec3{50, 50, 50} : Vec3{0.1f, 0.1f, 0.1f}; };
    auto start = std::chrono::steady_clock::now();
    Environment env = environment::build("spot", 1024, 512, sky(1024, 512, spot));
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 5.0);   // (a few tenths on a desktop)
    Vec3 at{0, 0, -1}, beside = normalize(Vec3{0.5f, 0, -1});   // 27 degrees away
    float centre0 = environment::sample(env.radiance[0], at).x, centre5 = environment::sample(env.radiance[5], at).x;
    CHECK(centre0 > centre5);
    CHECK(environment::sample(env.radiance[0], beside).x < 0.2f);
    CHECK(environment::sample(env.radiance[3], beside).x > environment::sample(env.radiance[1], beside).x);
    CHECK(env.sky.width == 1024 && env.sky.height == 512);
    Environment big = environment::build("big", 4096, 2048, std::vector<float>(4096u * 2048u * 3u, 0.5f));
    CHECK(big.sky.width == 2048 && big.sky.height == 1024);   // (at most 2048 wide)
}

TEST(environment_load_errors) {
    std::string error;
    CHECK(environment::load(test::tempPath("nope.hdr").u8string(), error) == nullptr);
    CHECK(!error.empty());
}

TEST(environment_from_probe_faces) {
    // Six flat faces, each its own colour (half floats: 1 = 0x3C00, 0.5 = 0x3800); the panorama looking along each
    // face's direction shows that face's colour
    const uint32_t size = 8;
    const uint16_t one = 0x3C00, half = 0x3800;
    const uint16_t colours[6][3] = {{one, 0, 0}, {0, one, 0}, {0, 0, one}, {half, 0, 0}, {0, half, 0}, {0, 0, half}};
    std::vector<uint16_t> px((size_t)size * size * 4 * 6, 0);
    for (int f = 0; f < 6; ++f)
        for (uint32_t i = 0; i < size * size; ++i)
            for (int c = 0; c < 3; ++c) px[((size_t)f * size * size + i) * 4 + (size_t)c] = colours[f][c];
    Environment env = environment::fromProbeFaces(px, size);
    REQUIRE(env.sky.width == 512 && env.sky.height == 256);
    for (int f = 0; f < 6; ++f) {
        Vec3 want = environment::probeFaceForward(f);
        size_t best = 0;
        float bestDot = -2.f;
        for (uint32_t y = 0; y < env.sky.height; ++y)
            for (uint32_t x = 0; x < env.sky.width; ++x) {
                float d = dot(environment::directionAt((x + 0.5f) / env.sky.width, (y + 0.5f) / env.sky.height), want);
                if (d > bestDot) bestDot = d, best = (size_t)y * env.sky.width + x;
            }
        float expect[3] = {colours[f][0] == one ? 1.f : colours[f][0] == half ? 0.5f : 0.f,
                           colours[f][1] == one ? 1.f : colours[f][1] == half ? 0.5f : 0.f,
                           colours[f][2] == one ? 1.f : colours[f][2] == half ? 0.5f : 0.f};
        for (int c = 0; c < 3; ++c) CHECK_NEAR(env.sky.rgb[best * 3 + (size_t)c], expect[c], 1e-3);
    }
    // Nothing read back (a size of 0): black, not a crash
    Environment none = environment::fromProbeFaces({}, 0);
    CHECK(none.sky.rgb.size() == 512u * 256u * 3u);
}
