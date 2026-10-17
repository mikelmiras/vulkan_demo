// Lights with a shape (shaders/scene.frag's lamp loop): a bulb `size` m round and a tube `length` m long, lit by
// Karis's representative points ("Real Shading in Unreal Engine 4", 2013): the diffuse light from the tube's nearest
// point, the highlight from the point of the light nearest the reflected ray (on the tube's segment, then on its
// sphere), with the highlight's height scaled down so a wider light's energy spreads rather than grows. Plain
// functions, tested; the shader repeats them.
#pragma once

#include "core/vecmath.h"

namespace arealight {

// The point of segment a..b nearest p
Vec3 nearestOnSegment(Vec3 a, Vec3 b, Vec3 p);

// The point of the segment from l0 to l1 (relative to the shaded point) nearest the ray along r (unit) from it
Vec3 segmentNearestRay(Vec3 l0, Vec3 l1, Vec3 r);

// The point of a sphere `radius` round c (relative to the shaded point) nearest the ray along r (unit)
Vec3 sphereNearestRay(Vec3 c, Vec3 r, float radius);

// The GGX highlight's scale for a light `radius` round and `length` long, `distance` away, at roughness alpha
// (roughness squared): the sphere's (alpha / alpha')^2, the tube's alpha / alpha'', 1 for a point
float highlightScale(float alpha, float radius, float length, float distance);

}  // namespace arealight
