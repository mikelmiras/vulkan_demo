// Temporal AA's history (shaders/taa.frag): how it's read where this pixel was last frame (Catmull-Rom: sharper
// than a bilinear tap, which softens the picture a little more every frame it moves) and how much of it is kept.
// Plain functions, tested; the shader repeats them.
#pragma once

#include <array>

#include "core/vecmath.h"

namespace taa {

// The four texels' weights round a sample a fraction `f` (0..1) past the second one, along one axis
// (Catmull-Rom: sums to one, passes through the texels, follows a straight ramp exactly)
std::array<float, 4> catmullRom(float f);

// How much history is kept: `base` (the still picture's), less the further the pixel moved since last frame
// (`movedPixels`: the history's taps then fall between texels, and fast motion wants the new frame's detail) and
// the less the history matched what's around the pixel now (`mismatch` 0..1: how far it lay outside their colours'
// box, relative to the box's size)
float historyWeight(float base, float movedPixels, float mismatch);

// The box the history is clipped to, from the pixel and its four neighbours (the cross; colours compressed): their
// range, narrowed to 1.25 standard deviations round their mean (variance clipping)
void neighbourhoodBox(const std::array<Vec3, 5>& colours, Vec3& lo, Vec3& hi);

// The composite's contrast-adaptive sharpening (after FidelityFX CAS) of one channel, compressed (0..1): the centre
// pushed away from its four neighbours by as much as their contrast leaves room for; `amount` 0..1
float casSharpen(float centre, float up, float left, float right, float down, float amount);

}  // namespace taa
