// The physically based sky (RenderSettings::physicalSky): Earth's atmosphere as Rayleigh scattering (air: blue),
// Mie scattering (haze: white, forward) and ozone absorption, after Hillaire's "A Scalable and Production Ready Sky
// and Atmosphere Rendering Technique" (2020). The GPU draws the sky from look-up tables (shaders/atmosphere.glsl,
// renderer_sky.cpp); these are the same sums on the CPU, for what's needed once a frame: the sun's colour once
// through the air, the sky's colours that light the scene, and where the sun is at a time of day.
#pragma once

#include "core/vecmath.h"

namespace atmosphere {

// Distances in km. The planet's radius, the atmosphere's top, and the viewer's height above the ground (fixed: the
// scenes are small, and the sky looks the same anywhere in them)
constexpr float kGroundRadius = 6360.f, kTopRadius = 6460.f, kViewHeight = 0.2f;

// How much light is left after going from `height` (km above the ground) out of the atmosphere, towards a
// direction `cosZenith` from straight up (rgb, 0..1; 0 where the planet is in the way). `haze` scales the Mie part.
Vec3 transmittance(float height, float cosZenith, float haze = 1.f);
// The sky's radiance looking along `dir` (normalized) from the viewer, for a sun towards `toSun` (normalized) of
// irradiance 1 above the atmosphere (single scattering: what the sky look-up table holds)
Vec3 skyRadiance(Vec3 dir, Vec3 toSun, float haze = 1.f);

// Where the sun is (a direction towards it, y up) at `hours` (0..24; 12: highest, in the south for a northern
// `latitude`, degrees) on an equinox: rising in the east (+x) at 6, setting in the west (-x) at 18; north is -z,
// turned about the vertical by `turn` radians. Below the horizon at night.
Vec3 sunDirection(float hours, float latitude, float turn = 0.f);

// What the sky lights the scene with, for a sun of irradiance 1 towards `toSun`: the sun's colour once through the
// air, the sky straight up and its average just above the horizon (linear)
struct SkyLight {
    Vec3 sun, zenith, horizon;
};
SkyLight skyLight(Vec3 toSun, float haze = 1.f);

// How much the air dims light per km at the ground (rgb): aerial perspective's
Vec3 seaLevelExtinction(float haze = 1.f);

}  // namespace atmosphere
