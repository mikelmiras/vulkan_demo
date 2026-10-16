// The physically based sky's model on the CPU (render/atmosphere.h): the sun through the air, the sky's colours, the
// sun's path through the day; and its settings saved with a scene
#include "test.h"

#include <cmath>

#include "render/atmosphere.h"
#include "scene/scene.h"
#include "scene/serialization.h"

using namespace atmosphere;

namespace {
Vec3 sunAt(float elevationDeg) {
    float e = elevationDeg * DEG2RAD;
    return {0.f, std::sin(e), std::cos(e)};
}
}  // namespace

TEST(atmosphere_sun_reddens_towards_the_horizon) {
    // Overhead: most of the light gets through, a little more red than blue (blue is scattered: the sky)
    Vec3 high = transmittance(kViewHeight, 1.f);
    CHECK(high.x > 0.9f && high.x > high.y && high.y > high.z && high.z > 0.7f);
    // Low: far less, and red (sunset); below the horizon: none
    Vec3 low = transmittance(kViewHeight, std::sin(2.f * DEG2RAD));
    CHECK(low.x < high.x && low.x > 0.2f && low.z < 0.1f * low.x);
    Vec3 below = transmittance(kViewHeight, std::sin(-5.f * DEG2RAD));
    CHECK(below.x == 0.f && below.z == 0.f);
    // Above the air: everything
    Vec3 top = transmittance(kTopRadius - kGroundRadius + 1.f, 0.f);
    CHECK_NEAR(top.x, 1.0, 1e-6);
    // Haze dims it more (and greyer: Mie's takes every colour alike)
    Vec3 hazy = transmittance(kViewHeight, 0.5f, 4.f), clear = transmittance(kViewHeight, 0.5f, 1.f);
    CHECK(hazy.x < clear.x && hazy.z < clear.z);
    CHECK(hazy.z / hazy.x > 0.5f * (clear.z / clear.x));
}

TEST(atmosphere_sky_is_blue_by_day_and_dark_at_night) {
    SkyLight noon = skyLight(sunAt(60.f));
    CHECK(noon.zenith.z > noon.zenith.y && noon.zenith.y > noon.zenith.x);   // (blue overhead)
    CHECK(noon.horizon.x + noon.horizon.y + noon.horizon.z > noon.zenith.x + noon.zenith.y + noon.zenith.z);   // (paler)
    CHECK(noon.zenith.z > 0.005f && noon.zenith.z < 0.2f);   // (for a sun of irradiance 1)
    // Sunset: the light reaching the ground red, the horizon warmer than at noon
    SkyLight dusk = skyLight(sunAt(2.f));
    CHECK(dusk.sun.x > 4.f * dusk.sun.z);
    CHECK(dusk.horizon.x / dusk.horizon.z > noon.horizon.x / noon.horizon.z);
    // Night: (almost) nothing
    SkyLight night = skyLight(sunAt(-15.f));
    CHECK(night.sun.x == 0.f);
    CHECK(night.zenith.z < 0.01f * noon.zenith.z && night.horizon.z < 0.01f * noon.horizon.z);
    // Brighter towards the sun than away from it (Mie's forward glow)
    Vec3 toSun = sunAt(10.f);
    Vec3 towards = skyRadiance(normalize(Vec3{0.f, 0.25f, 1.f}), toSun), away = skyRadiance(normalize(Vec3{0.f, 0.25f, -1.f}), toSun);
    CHECK(towards.x > 2.f * away.x);
}

TEST(atmosphere_multiple_scattering_and_harmonics) {
    // Light scattered more than once: there by day (bluish), more near the ground than high up, none at night
    Vec3 day = multipleScattering(kViewHeight, 0.7f), high = multipleScattering(60.f, 0.7f), night = multipleScattering(kViewHeight, -0.5f);
    CHECK(day.z > day.x && day.x > 0.f);
    CHECK(high.z < day.z);
    CHECK(night.z < 0.01f * day.z);
    // The sky as harmonics: positive on average, brighter overhead than at the horizon's average (y's term up),
    // brighter on the sun's side (here +z)
    Vec3 sh[9];
    skyHarmonics(normalize(Vec3{0.f, 0.5f, 1.f}), 1.f, sh);
    CHECK(sh[0].z > 0.f && sh[0].x > 0.f);
    CHECK(sh[1].z > 0.f);
    CHECK(sh[2].x > 0.f);
    float basis[9];
    harmonics({0.f, 1.f, 0.f}, basis);
    CHECK_NEAR(basis[0], 0.282095, 1e-5);
    CHECK_NEAR(basis[1], 0.488603, 1e-5);
}

TEST(atmosphere_sun_path) {
    // Equinox noon: due south (+z, north being -z) at 90 - latitude degrees up; on the equator, straight up
    Vec3 noon = sunDirection(12.f, 40.f);
    CHECK_NEAR(std::asin(noon.y) / DEG2RAD, 50.0, 1e-3);
    CHECK(noon.z > 0.f && std::fabs(noon.x) < 1e-5f);
    CHECK_NEAR(sunDirection(12.f, 0.f).y, 1.0, 1e-5);
    // Rises in the east (+x) at 6, sets in the west at 18, below the horizon at midnight
    Vec3 rise = sunDirection(6.f, 40.f), set = sunDirection(18.f, 40.f);
    CHECK_NEAR(rise.x, 1.0, 1e-5);
    CHECK_NEAR(set.x, -1.0, 1e-5);
    CHECK(sunDirection(9.f, 40.f).y > 0.f && sunDirection(9.f, 40.f).x > 0.f);
    CHECK(sunDirection(0.f, 40.f).y < 0.f);
    // Turned: the compass with it
    Vec3 turned = sunDirection(6.f, 40.f, 0.5f * PI);
    CHECK_NEAR(std::fabs(turned.z), 1.0, 1e-5);
    CHECK_NEAR(length(turned), 1.0, 1e-5);
}

TEST(atmosphere_settings_saved) {
    Scene s;
    s.render.physicalSky = true;
    s.render.timeOfDay = 17.5f;
    s.render.latitude = 55.f;
    s.render.haze = 2.f;
    s.render.aerialPerspective = 25.f;
    s.render.skyIntensity = 1.5f;
    s.render.skyRotation = 30.f;
    s.render.cloudCover = 0.4f;
    MeshLibrary meshes = buildMeshLibrary();
    std::string error;
    auto path = test::tempPath("sky.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    CHECK(loaded.render.physicalSky);
    CHECK_NEAR(loaded.render.timeOfDay, 17.5, 1e-6);
    CHECK_NEAR(loaded.render.latitude, 55.0, 1e-6);
    CHECK_NEAR(loaded.render.haze, 2.0, 1e-6);
    CHECK_NEAR(loaded.render.aerialPerspective, 25.0, 1e-6);
    CHECK_NEAR(loaded.render.skyIntensity, 1.5, 1e-6);   // (kept without a sky image: the physical sky's)
    CHECK_NEAR(loaded.render.skyRotation, 30.0, 1e-6);
    CHECK_NEAR(loaded.render.cloudCover, 0.4, 1e-6);
    Scene plain;
    CHECK(!plain.render.physicalSky && plain.render.timeOfDay < 0.f);   // (off unless asked for: the colours)
}

TEST(atmosphere_clouds_in_the_ambient) {
    // The clouds' average: none without cover, more with more, most of the sky when overcast
    CHECK(cloudFraction(0.f) == 0.f);
    float some = cloudFraction(0.3f), more = cloudFraction(0.6f), all = cloudFraction(1.f);
    CHECK(some > 0.02f && some < more && more < all);
    CHECK(all > 0.8f && all <= 1.f);
    // Over the sky's harmonics: none leaves them; all of it makes them the clouds' alone (even all round: no pull
    // towards the sun's side), grey where the sky was blue
    Vec3 sky[9], clear[9];
    skyHarmonics(normalize(Vec3{0.f, 0.5f, 1.f}), 1.f, sky);
    for (int k = 0; k < 9; ++k) clear[k] = sky[k];
    cloudyHarmonics(sky, 0.f, {0.1f, 0.1f, 0.1f});
    CHECK_NEAR(sky[2].z, clear[2].z, 1e-7);
    cloudyHarmonics(sky, 1.f, {0.1f, 0.1f, 0.1f});
    float upper[9];
    hemisphereHarmonics(true, upper);
    CHECK_NEAR(sky[0].x, 0.1 * upper[0], 1e-6);
    CHECK_NEAR(sky[0].z, sky[0].x, 1e-6);
    CHECK(std::fabs(sky[2].z) < 1e-3f && std::fabs(sky[3].x) < 1e-3f);
    // The two halves add up to the whole sphere's: its constant term alone (sqrt(4 pi))
    float lower[9];
    hemisphereHarmonics(false, lower);
    CHECK_NEAR(upper[0] + lower[0], std::sqrt(4.0 * PI), 1e-3);
    CHECK_NEAR(upper[1], -lower[1], 1e-3);
    CHECK(std::fabs(upper[2] + lower[2]) < 1e-3f);
}

TEST(atmosphere_probes_rebaked_when_the_sky_moves) {
    SkyState baked;
    baked.toSun = sunDirection(10.f, 40.f);
    SkyState now = baked;
    CHECK(!skyChangedEnough(baked, now));
    now.toSun = sunDirection(10.05f, 40.f);   // (three minutes: under a degree)
    CHECK(!skyChangedEnough(baked, now));
    now.toSun = sunDirection(10.5f, 40.f);    // (half an hour)
    CHECK(skyChangedEnough(baked, now));
    now = baked;
    now.cloudCover = 0.03f;
    CHECK(!skyChangedEnough(baked, now));
    now.cloudCover = 0.2f;
    CHECK(skyChangedEnough(baked, now));
    now = baked;
    now.haze = 1.2f;
    CHECK(skyChangedEnough(baked, now));
}
