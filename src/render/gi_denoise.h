// Screen-space GI's bounce denoised (shaders/gi_denoise.frag): each of the 3 x 3 taps' weight, from where it is in
// the kernel, how near its distance is to the centre's and how alike its normal is. A plain function, tested; the
// shader repeats it.
#pragma once

namespace gidenoise {

// A tap `x`, `y` (-1..1) from the centre at distance z, the tap at distance tapZ, the normals' dot product
float tapWeight(int x, int y, float z, float tapZ, float normalDot);

}  // namespace gidenoise
