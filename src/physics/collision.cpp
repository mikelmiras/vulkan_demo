#include "physics/collision.h"

#include <cfloat>

namespace collision {

Vec3 closestPointOnSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = b - a;
    float len2 = dot(ab, ab);
    if (len2 < 1e-12f) return a;
    float t = std::clamp(dot(p - a, ab) / len2, 0.f, 1.f);
    return a + ab * t;
}

// Ericson, Real-Time Collision Detection 5.1.5
Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) return a;
    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return a + ab * (d1 / (d1 - d3));
    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float denom = 1.f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

Vec3 closestPointOnObb(Vec3 p, const Obb& box) {
    Vec3 d = p - box.center, q = box.center;
    const float half[3] = {box.half.x, box.half.y, box.half.z};
    for (int i = 0; i < 3; ++i) q += box.axis[i] * std::clamp(dot(d, box.axis[i]), -half[i], half[i]);
    return q;
}

bool sphereVsSphere(const Sphere& s, const Sphere& other, Contact& out) {
    Vec3 d = s.center - other.center;
    float dist = length(d), r = s.radius + other.radius;
    if (dist >= r) return false;
    out.normal = dist > 1e-6f ? d * (1.f / dist) : Vec3{0, 1, 0};
    out.depth = r - dist;
    return true;
}

bool sphereVsObb(const Sphere& s, const Obb& box, Contact& out) {
    Vec3 d = s.center - box.center;
    const float half[3] = {box.half.x, box.half.y, box.half.z};
    float local[3];
    bool inside = true;
    for (int i = 0; i < 3; ++i) {
        local[i] = dot(d, box.axis[i]);
        if (std::fabs(local[i]) > half[i]) inside = false;
    }
    if (!inside) {
        Vec3 q = closestPointOnObb(s.center, box), v = s.center - q;
        float dist = length(v);
        if (dist >= s.radius) return false;
        out.normal = v * (1.f / dist);
        out.depth = s.radius - dist;
        return true;
    }
    // Centre inside the box: leave through the nearest face
    int best = 0;
    float bestGap = FLT_MAX;
    for (int i = 0; i < 3; ++i) {
        float gap = half[i] - std::fabs(local[i]);
        if (gap < bestGap) { bestGap = gap; best = i; }
    }
    out.normal = box.axis[best] * (local[best] >= 0.f ? 1.f : -1.f);
    out.depth = bestGap + s.radius;
    return true;
}

bool sphereVsTriangle(const Sphere& s, Vec3 a, Vec3 b, Vec3 c, Contact& out) {
    Vec3 q = closestPointOnTriangle(s.center, a, b, c), v = s.center - q;
    float dist2 = dot(v, v);
    if (dist2 >= s.radius * s.radius) return false;
    float dist = std::sqrt(dist2);
    if (dist > 1e-6f) {
        out.normal = v * (1.f / dist);
    } else {   // centre on the triangle: use its face normal (towards the side the sphere came from is unknown)
        out.normal = normalize(cross(b - a, c - a));
    }
    out.depth = s.radius - dist;
    return true;
}

bool sphereVsCapsule(const Sphere& s, const Capsule& c, Contact& out) {
    return sphereVsSphere(s, Sphere{closestPointOnSegment(s.center, c.a, c.b), c.radius}, out);
}

bool obbVsObb(const Obb& a, const Obb& b) {
    // 15 candidate axes: 3 + 3 face normals and 9 edge cross products
    Vec3 axes[15];
    int n = 0;
    for (int i = 0; i < 3; ++i) axes[n++] = a.axis[i];
    for (int i = 0; i < 3; ++i) axes[n++] = b.axis[i];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            Vec3 c = cross(a.axis[i], b.axis[j]);
            if (dot(c, c) > 1e-8f) axes[n++] = normalize(c);
        }
    Vec3 d = b.center - a.center;
    const float ha[3] = {a.half.x, a.half.y, a.half.z}, hb[3] = {b.half.x, b.half.y, b.half.z};
    for (int k = 0; k < n; ++k) {
        float ra = 0.f, rb = 0.f;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] * std::fabs(dot(a.axis[i], axes[k]));
            rb += hb[i] * std::fabs(dot(b.axis[i], axes[k]));
        }
        if (std::fabs(dot(d, axes[k])) > ra + rb) return false;
    }
    return true;
}

bool obbContact(const Obb& a, const Obb& b, Contact& out) {
    Vec3 axes[15];
    int n = 0;
    for (int i = 0; i < 3; ++i) axes[n++] = a.axis[i];
    for (int i = 0; i < 3; ++i) axes[n++] = b.axis[i];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            Vec3 c = cross(a.axis[i], b.axis[j]);
            if (dot(c, c) > 1e-6f) axes[n++] = normalize(c);
        }
    Vec3 d = a.center - b.center;
    const float ha[3] = {a.half.x, a.half.y, a.half.z}, hb[3] = {b.half.x, b.half.y, b.half.z};
    float best = FLT_MAX;
    for (int k = 0; k < n; ++k) {
        float ra = 0.f, rb = 0.f;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] * std::fabs(dot(a.axis[i], axes[k]));
            rb += hb[i] * std::fabs(dot(b.axis[i], axes[k]));
        }
        float dist = dot(d, axes[k]), overlap = ra + rb - std::fabs(dist);
        if (overlap <= 0.f) return false;   // separated along this axis
        // Face axes win ties against edge axes (k < 6): resting boxes get clean up/down normals
        if (overlap < best - (k < 6 ? 0.f : 1e-4f)) {
            best = overlap;
            out.normal = dist >= 0.f ? axes[k] : axes[k] * -1.f;
        }
    }
    out.depth = best;
    return true;
}

float rayVsSphere(Vec3 origin, Vec3 dir, const Sphere& s, Vec3& normal) {
    Vec3 m = origin - s.center;
    float b = dot(m, dir), c = dot(m, m) - s.radius * s.radius;
    if (c > 0.f && b > 0.f) return -1.f;
    float disc = b * b - c;
    if (disc < 0.f) return -1.f;
    float t = std::max(-b - std::sqrt(disc), 0.f);   // starting inside counts as a hit at 0
    normal = normalize(origin + dir * t - s.center);
    return t;
}

float rayVsObb(Vec3 origin, Vec3 dir, const Obb& box, Vec3& normal) {
    float tmin = 0.f, tmax = FLT_MAX;
    Vec3 d = origin - box.center;
    const float half[3] = {box.half.x, box.half.y, box.half.z};
    int hitAxis = -1;
    float hitSign = 1.f;
    for (int i = 0; i < 3; ++i) {
        float o = dot(d, box.axis[i]), v = dot(dir, box.axis[i]);
        if (std::fabs(v) < 1e-9f) {
            if (std::fabs(o) > half[i]) return -1.f;
            continue;
        }
        float t1 = (-half[i] - o) / v, t2 = (half[i] - o) / v;
        float sign = -1.f;   // entering through the -axis face
        if (t1 > t2) { std::swap(t1, t2); sign = 1.f; }
        if (t1 > tmin) { tmin = t1; hitAxis = i; hitSign = sign; }
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return -1.f;
    }
    normal = hitAxis >= 0 ? box.axis[hitAxis] * hitSign : dir * -1.f;   // -1: started inside
    return tmin;
}

}  // namespace collision
