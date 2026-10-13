// Volumetric fog (render/renderer_fog.cpp): the grid of cells through the view, shared by its two compute passes
// and the shaders reading the result. Cells are 160 x 90 across the view, 64 slices deep, spaced logarithmically
// from fogRange.x to fogRange.z along the view.
const ivec3 kFogCells = ivec3(160, 90, 64);

// Where slice s (fractional) starts, metres along the view
float fogSliceDepth(float s) { return frame.fogRange.x * exp(s / frame.fogRange.y); }
// The fog volume's texture coordinate (z) for a distance along the view: its texels hold what's between the camera
// and the end of their slice
float fogVolumeZ(float depth) {
    float s = log(max(depth, 1e-3) / frame.fogRange.x) * frame.fogRange.y;
    return clamp((s - 0.5) / float(kFogCells.z), 0.0, 1.0);
}
// Henyey-Greenstein, times 4 pi (so light scattered alike every way, g = 0, counts as 1): how much of light going
// along `travel` turns towards the camera, the view looking along `view`
float fogPhase(vec3 travel, vec3 view, float g) {
    float c = dot(travel, -view);
    float d = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (d * sqrt(d));
}
