// Camera effects' CPU side (shaders/camera_fx.glsl): the sky's motion for motion blur. The sky has no motion vectors
// (nothing's drawn there), but it moves on screen when the camera turns: a direction's place on screen now and last
// frame are related by a 3 x 3 matrix (the two view-projections' rotation and projection parts; positions drop
// out at infinity). And the weights the shader gathers with, repeated here to test them. Plain functions, tested;
// the post pass passes the matrix in push constants (renderer_post.cpp).
#pragma once

#include "core/vecmath.h"

namespace camerafx {

// H, row major: (x, y, 1) in this frame's clip space (x, y in -1..1) times H gives last frame's (divide by z) for
// a direction (the sky). Identity if the camera only moved, didn't turn (or didn't change at all).
void skyMotion(const Mat4& viewProjNow, const Mat4& viewProjBefore, float h[9]);

// Motion blur's weight for a tap `distance` pixels along the blur from a pixel moving `here` pixels a frame (the
// shutter's share), the tap's own thing moving `there`, `front` how much the tap is in front (0..1, soft): it counts
// if it's in front and its motion spans the gap (smearing over this pixel), behind and this pixel's does (this
// pixel's smear over it), or both do (McGuire et al. 2012)
float motionTapWeight(float distance, float here, float there, float front);

// Depth of field: how much of a pixel the near field (blurred, in front) covers, from its taps' weights `covered`
// out of `taps` spread evenly over the disc (the share of the disc it fills)
float nearCoverage(float covered, int taps);

}  // namespace camerafx
