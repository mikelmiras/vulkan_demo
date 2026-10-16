// Camera effects' CPU side (shaders/camera_fx.glsl): the sky's motion for motion blur. The sky has no motion vectors
// (nothing's drawn there), but it moves on screen when the camera turns: a direction's place on screen now and last
// frame are related by a 3 x 3 matrix (the two view-projections' rotation and projection parts; positions drop
// out at infinity). A plain function, tested; the post pass passes it in push constants (renderer_post.cpp).
#pragma once

#include "core/vecmath.h"

namespace camerafx {

// H, row major: (x, y, 1) in this frame's clip space (x, y in -1..1) times H gives last frame's (divide by z) for
// a direction (the sky). Identity if the camera only moved, didn't turn (or didn't change at all).
void skyMotion(const Mat4& viewProjNow, const Mat4& viewProjBefore, float h[9]);

}  // namespace camerafx
