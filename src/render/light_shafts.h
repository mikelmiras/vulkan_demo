// Light shafts (shaders/shafts.glsl: the clear air's in-scattering marched along each view ray through the sun's
// cascades and the shadowed spot lights' beams): what the march gets, from the scene's settings and the lights. A
// plain function, tested; the renderer fills the frame's shafts from it (renderer.cpp).
#pragma once

#include <vector>

namespace shafts {

struct Params {
    float density = 0.f;           // scattering per metre (0: none)
    int lamps[2] = {-1, -1};       // spot lights' indices in the frame's lights (-1: none)
    int steps = 0;                 // along each ray
};

// `strength`: RenderSettings::lightShafts (0..2); off with volumetric fog (it has its own) and at low quality
// (0 low, 1 medium, 2 high: 16 steps, 24 at high). `spots[i]`: the frame's light i is a spot light with a shadow;
// the first two (the nearest: the lights come nearest first) are marched.
Params choose(float strength, bool volumetricFog, int quality, const std::vector<bool>& spots);

}  // namespace shafts
