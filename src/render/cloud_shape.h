// Volumetric clouds' shape (shaders/clouds.glsl's cloudAt): the density at a height through the cloud slab from the
// flat layer's coverage there: a flat base, and a dome over it as tall as the cover is dense (thin cover stays low
// and wide; the densest reaches the top). A plain function, tested; the shader repeats it.
#pragma once

#include "core/vecmath.h"

namespace clouds {

// `cover` 0..1 (cloudDensity's), `h` 0..1 through the slab (base to top)
float density(float cover, float h);

// Where a direction was on last frame's screen (shaders/clouds.frag's history, the same sums): through last frame's
// camera as a direction (w = 0: kilometres off, the camera's move doesn't count), uv 0..1 down the screen; false
// when it was behind or off the picture
bool historyUv(const Mat4& prevViewProj, Vec3 dir, float& u, float& v);

}  // namespace clouds
