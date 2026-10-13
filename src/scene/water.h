// Water's waves (the Water component, scene/scene.h): three Gerstner waves (the main one, one turned 35 degrees and
// 0.57 as long, one turned -25 degrees and 0.31 as long), each point of the surface going round in a circle, more
// pointed crests the choppier it is. The same sums as shaders/water.glsl (this is their reference, for tests), what the
// physics floats things on (water.h's height and velocity), and the drawn surface's mesh.
#pragma once

#include "core/vecmath.h"
#include "scene/scene.h"

struct MeshLibrary;

namespace water {

// The surface point that starts (at rest) at `at` (world x, z), `t` seconds in: where it's moved (x, y, z from rest,
// y up from the level), its normal and how fast it's going. `yaw`: the entity's turn round the vertical (radians).
struct Sample {
    Vec3 offset;
    Vec3 normal{0.f, 1.f, 0.f};
    Vec3 velocity;
};
Sample sample(const Water& w, float yaw, float x, float z, float t);
// The surface's height (above the level) right over (x, z): the rest point whose moved position lands there, found by
// two fixed-point steps
float heightAt(const Water& w, float yaw, float x, float z, float t);

// The drawn grid (shared by every Water: a unit square of 128 x 128 quads in xz, centred), made once
void syncMeshes(Scene& scene, MeshLibrary& meshes);

}  // namespace water
