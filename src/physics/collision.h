// Collision primitives in world space, and the contact / ray tests between them.
#pragma once

#include "core/vecmath.h"

namespace collision {

struct Sphere {
    Vec3 center;
    float radius = 0.5f;
};

// Oriented box: unit axes and half extents along them
struct Obb {
    Vec3 center;
    Vec3 axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 half{0.5f, 0.5f, 0.5f};
};

// Segment a-b swept by a sphere
struct Capsule {
    Vec3 a, b;
    float radius = 0.5f;
};

// How to separate two shapes: move the first one by normal * depth
struct Contact {
    Vec3 normal;
    float depth = 0.f;
};

Vec3 closestPointOnSegment(Vec3 p, Vec3 a, Vec3 b);
Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);
Vec3 closestPointOnObb(Vec3 p, const Obb& box);

// Contact tests: true (and the separating contact for the first shape) when they overlap
bool sphereVsSphere(const Sphere& s, const Sphere& other, Contact& out);
bool sphereVsObb(const Sphere& s, const Obb& box, Contact& out);
bool sphereVsTriangle(const Sphere& s, Vec3 a, Vec3 b, Vec3 c, Contact& out);
bool sphereVsCapsule(const Sphere& s, const Capsule& c, Contact& out);
bool obbVsObb(const Obb& a, const Obb& b);   // overlap only (separating axis test)
bool obbContact(const Obb& a, const Obb& b, Contact& out);   // with the axis of least overlap as the normal

// Capsules are tested as a row of spheres along the segment (exact enough for characters and triggers);
// calls fn(sphere) for each and stops early if it returns false
template <class F>
void forEachCapsuleSphere(const Capsule& c, F&& fn) {
    float len = length(c.b - c.a);
    int n = len > 1e-5f ? 1 + (int)std::ceil(len / (c.radius * 0.5f)) : 1;
    for (int i = 0; i < n; ++i) {
        float t = n == 1 ? 0.f : (float)i / (float)(n - 1);
        if (!fn(Sphere{c.a + (c.b - c.a) * t, c.radius})) return;
    }
}

// Ray tests: distance along the (unit) direction, or -1; the surface normal at the hit
float rayVsSphere(Vec3 origin, Vec3 dir, const Sphere& s, Vec3& normal);
float rayVsObb(Vec3 origin, Vec3 dir, const Obb& box, Vec3& normal);

}  // namespace collision
