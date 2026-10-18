// Screen-space GI's bounce denoised (shaders/gi_denoise.frag): each of the 3 x 3 taps' weight, from where it is in
// the kernel, how near its distance is to the centre's and how alike its normal is. A plain function, tested; the
// shader repeats it.
#pragma once

namespace gidenoise {

// A tap `x`, `y` (-1..1) from the centre at distance z, the tap at distance tapZ, the normals' dot product
float tapWeight(int x, int y, float z, float tapZ, float normalDot);

// How much of the traced reflections' wider ring a surface this rough blends in (scene.frag's readTraced: half of
// it at full share): none on a mirror or wet asphalt (below 0.15: their reflections stay sharp), all from 0.4 up
float ssrBlurShare(float roughness);

}  // namespace gidenoise
