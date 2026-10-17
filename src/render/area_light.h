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

// A rectangle centred on c (relative to the shaded point), its half extents hx along x and hy along y (unit, at
// right angles): its point nearest the shaded point (the origin)
Vec3 rectNearest(Vec3 c, Vec3 x, Vec3 y, float hx, float hy);

// Its point the ray along r (unit) from the shaded point meets, kept inside the rectangle; a ray parallel to it or
// going away: the rectangle's point nearest the ray's direction a long way off
Vec3 rectRayPoint(Vec3 c, Vec3 x, Vec3 y, float hx, float hy, Vec3 r);

// The GGX highlight's scale for a light `radius` round and `length` long, `distance` away, at roughness alpha
// (roughness squared): the sphere's (alpha / alpha')^2, the tube's alpha / alpha'', 1 for a point
float highlightScale(float alpha, float radius, float length, float distance);

// Where on a rectangle's picture a point of it is (scene.frag's, the same sums): `rel` from its middle, `x` its width's
// axis (the entity's x), `y` its height's, `w` and `h` its size; u runs along -x so the picture reads the right way
// round to someone in front of it (on the side it lights), v down from the top
void pictureUv(Vec3 rel, Vec3 x, Vec3 y, float w, float h, float& u, float& v);
// The picture's level of blur (its mips, 256 across) for the light reaching `distance` away: the further, the more of
// it averaged, all of it once as far as its size
float pictureLod(float distance, float size);

}  // namespace arealight
