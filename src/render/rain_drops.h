// Rain on glass and on clear-coated paint (shaders/scene.frag's rainDrops repeats these sums): beads sitting on the
// surface, and on steep ones drops running down it at their own paces, each leaving a short wet trail above it. A
// plain function of a point in the surface's plane, tested; the shader bends the normal by its slope.
#pragma once

#include "core/vecmath.h"

namespace raindrops {

// At `q` (metres in the surface's plane: x along its horizontal, y up it), time `t` (seconds), rain 0..1 and how
// steep the surface is 0..1 (only steep ones have running drops): x, y = the water's slope there (a bead's dome
// leaning out from its middle), z = how wet the point is (1 under a drop, less along a trail, 0 dry)
Vec3 dropAt(float qx, float qy, float t, float rain, float steep);

}  // namespace raindrops
