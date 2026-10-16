// Light shafts (shaders/shafts.glsl: the clear air's in-scattering marched along each view ray through the sun's
// cascades and the lamps' light): what the march gets, from the scene's settings and the lights, and a point light's
// halo along a ray. Plain functions, tested; the renderer fills the frame's shafts from them (renderer.cpp).
#pragma once

#include <vector>

namespace shafts {

constexpr int kMaxLamps = 4;

struct Params {
    float density = 0.f;                        // scattering per metre (0: none); 0.4 of it the air's extinction
    int lamps[kMaxLamps] = {-1, -1, -1, -1};    // lamps' indices in the frame's lights (-1: none)
    int lampCount = 0;
    int steps = 0;                              // along each ray
};

// `strength`: RenderSettings::lightShafts (0..2); off with volumetric fog (it has its own) and at low quality
// (0 low, 1 medium, 2 high: 16 steps, 24 at high). `lamps[i]`: how much the frame's light i (a point or spot light)
// would show in the air (prominence; 0: not at all); the four that show most do, in the lights' order.
Params choose(float strength, bool volumetricFog, int quality, const std::vector<float>& lamps);

// How much a lamp shows in the air: its brightness over the space it lights (range squared), less with distance
// from the camera beyond that range (a street lamp's halo down the road before a tail light's at the car)
float prominence(float brightness, float range, float distance);

// The light a ray gathers passing a point light, without its falloff's window: the integral of
// 1 / (h^2 + s^2 + soft^2) over the ray's [t0, t1], s the distance along it from the closest point (at tc, h from
// the light). `soft` keeps it finite through the bulb: a glowing core that wide, not a spike. (shafts.glsl's halo)
float halo(float tc, float h, float t0, float t1, float soft);

}  // namespace shafts
