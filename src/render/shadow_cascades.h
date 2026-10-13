// The sun's shadow cascades for a camera: its view from 0.1 m out to a distance in four slices (closer ones smaller,
// so sharper), each an orthographic camera looking down the sun around a sphere holding its slice, snapped to the
// map's texels so edges don't shimmer as the camera moves. A plain function: the renderer calls it for the main
// view and each split-screen view (renderer_shadows.cpp), tests check it.
#pragma once

#include "core/vecmath.h"

namespace shadows {

struct Camera {
    Vec3 position, forward;
    float fovY = 1.f;      // radians
    float aspect = 1.f;
};

constexpr int kCascades = 4;

// `splits`: each slice's far end along the view (metres). `viewProj`: each cascade's camera, for a map `texels`
// across (only when `viewProj` isn't null)
void sunCascades(const Camera& camera, Vec3 sunDir, float distance, float texels, Mat4* viewProj, float* splits);

}  // namespace shadows
