// The reflections' walk over the near pyramid (shaders/screen_trace.glsl's traceReflection), in one dimension: a
// row of the picture's distances, a pyramid of the nearest of each 2, 4, 8, ... texels, and a ray going away along
// the row. Over a tile where nothing is as near as the ray, it jumps to the tile's end and goes up a level; where
// something is, down a level, down to single texels, where it checks for the hit. A plain function to test the walk
// (it finds what a texel-by-texel march finds, in fewer samples); the shader does the same in two dimensions.
#pragma once

#include <vector>

namespace hiztrace {

// The pyramid: level 0 the row itself, each level above the nearest of two texels below
std::vector<std::vector<float>> nearPyramid(const std::vector<float>& row);

struct Hit {
    int at = -1;       // the texel where the ray first passes behind what's there (-1: none)
    int samples = 0;   // texels or tiles looked at
};

// A ray from texel `from` (its middle) at distance z0, its distance growing dz per texel; levels up to `top`
Hit walk(const std::vector<std::vector<float>>& pyramid, int from, float z0, float dz, int top);

// The same, texel by texel
Hit march(const std::vector<float>& row, int from, float z0, float dz);

}  // namespace hiztrace
