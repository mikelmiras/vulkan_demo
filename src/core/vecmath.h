// Minimal math: column-major 4x4 matrices (same memory layout as OpenGL/ImGuizmo), right-handed.
#pragma once

#include <algorithm>
#include <cmath>

constexpr float PI = 3.14159265358979f;
constexpr float DEG2RAD = PI / 180.f;

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) { float l = length(a); return l > 0.f ? a * (1.f / l) : a; }
inline Vec3 srgbToLinear(Vec3 c) { return {std::pow(c.x, 2.2f), std::pow(c.y, 2.2f), std::pow(c.z, 2.2f)}; }

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline Mat4 translate(Vec3 t) { Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r; }

inline Mat4 rotateY(float a) {
    Mat4 r; float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}

// Rotation by `angle` radians around the unit vector `axis`
inline Mat4 rotateAxis(Vec3 axis, float angle) {
    float c = std::cos(angle), s = std::sin(angle), t = 1.f - c;
    float x = axis.x, y = axis.y, z = axis.z;
    Mat4 r;
    r.m[0] = t * x * x + c;     r.m[4] = t * x * y - s * z; r.m[8] = t * x * z + s * y;
    r.m[1] = t * x * y + s * z; r.m[5] = t * y * y + c;     r.m[9] = t * y * z - s * x;
    r.m[2] = t * x * z - s * y; r.m[6] = t * y * z + s * x; r.m[10] = t * z * z + c;
    return r;
}

// vulkanClip = true: Y down, depth [0,1]. false: OpenGL convention (what ImGuizmo expects).
inline Mat4 perspective(float fovy, float aspect, float zn, float zf, bool vulkanClip) {
    Mat4 r;
    float f = 1.f / std::tan(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = vulkanClip ? -f : f;
    r.m[11] = -1.f;
    r.m[15] = 0.f;
    if (vulkanClip) {
        r.m[10] = zf / (zn - zf);
        r.m[14] = zn * zf / (zn - zf);
    } else {
        r.m[10] = (zf + zn) / (zn - zf);
        r.m[14] = 2.f * zf * zn / (zn - zf);
    }
    return r;
}

// Orthographic projection of a right-handed view (looking down -Z) to Vulkan clip space: depth [0, 1]
inline Mat4 orthographic(float l, float r, float b, float t, float zn, float zf) {
    Mat4 m;
    m.m[0] = 2.f / (r - l);
    m.m[5] = 2.f / (t - b);
    m.m[10] = -1.f / (zf - zn);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -zn / (zf - zn);
    return m;
}

inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = normalize(center - eye), s = normalize(cross(f, up)), u = cross(s, f);
    Mat4 r;
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;   r.m[12] = -dot(s, eye);
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;   r.m[13] = -dot(u, eye);
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
    return r;
}

// Inverse of an affine matrix (rotation/scale/translation, last row 0 0 0 1)
inline Mat4 inverseAffine(const Mat4& a) {
    auto e = [&](int row, int col) { return a.m[col * 4 + row]; };
    float c00 = e(1, 1) * e(2, 2) - e(1, 2) * e(2, 1);
    float c01 = e(1, 2) * e(2, 0) - e(1, 0) * e(2, 2);
    float c02 = e(1, 0) * e(2, 1) - e(1, 1) * e(2, 0);
    float det = e(0, 0) * c00 + e(0, 1) * c01 + e(0, 2) * c02;
    float id = std::fabs(det) > 1e-12f ? 1.f / det : 0.f;

    float inv[3][3] = {
        {c00 * id, (e(0, 2) * e(2, 1) - e(0, 1) * e(2, 2)) * id, (e(0, 1) * e(1, 2) - e(0, 2) * e(1, 1)) * id},
        {c01 * id, (e(0, 0) * e(2, 2) - e(0, 2) * e(2, 0)) * id, (e(0, 2) * e(1, 0) - e(0, 0) * e(1, 2)) * id},
        {c02 * id, (e(0, 1) * e(2, 0) - e(0, 0) * e(2, 1)) * id, (e(0, 0) * e(1, 1) - e(0, 1) * e(1, 0)) * id},
    };
    Mat4 r;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.m[col * 4 + row] = inv[row][col];
    for (int row = 0; row < 3; ++row)
        r.m[12 + row] = -(inv[row][0] * a.m[12] + inv[row][1] * a.m[13] + inv[row][2] * a.m[14]);
    return r;
}

inline Vec3 transformPoint(const Mat4& a, Vec3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
            a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}

inline Vec3 transformDir(const Mat4& a, Vec3 d) {
    return {a.m[0] * d.x + a.m[4] * d.y + a.m[8] * d.z,
            a.m[1] * d.x + a.m[5] * d.y + a.m[9] * d.z,
            a.m[2] * d.x + a.m[6] * d.y + a.m[10] * d.z};
}

// Model matrix at `pos` whose local -Z points along `forward` (Y up where possible), with the given scale
inline Mat4 lookRotation(Vec3 pos, Vec3 forward, const float scale[3]) {
    Vec3 f = normalize(forward);
    Vec3 up = std::fabs(f.y) > 0.999f ? Vec3{0, 0, f.y > 0.f ? 1.f : -1.f} : Vec3{0, 1, 0};   // straight up/down
    Mat4 m = inverseAffine(lookAt(pos, pos + f, up));
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m.m[c * 4 + r] *= scale[c];
    return m;
}

// Rotations as unit quaternions (x, y, z, w), as glTF stores them: for animation
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
inline Quat normalizeQuat(Quat q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 0.f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
// Shortest-path spherical interpolation
inline Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.f) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f)   // nearly the same: a normalised lerp is exact enough and avoids dividing by ~0
        return normalizeQuat(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    float th = std::acos(d), s = std::sin(th), wa = std::sin((1.f - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
// Translation * rotation * scale, the glTF node order
inline Mat4 composeTRS(Vec3 t, Quat q, Vec3 s) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m;
    m.m[0] = (1.f - 2.f * (yy + zz)) * s.x; m.m[1] = 2.f * (xy + wz) * s.x;         m.m[2] = 2.f * (xz - wy) * s.x;
    m.m[4] = 2.f * (xy - wz) * s.y;         m.m[5] = (1.f - 2.f * (xx + zz)) * s.y; m.m[6] = 2.f * (yz + wx) * s.y;
    m.m[8] = 2.f * (xz + wy) * s.z;         m.m[9] = 2.f * (yz - wx) * s.z;         m.m[10] = (1.f - 2.f * (xx + yy)) * s.z;
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}
// The rotation of an affine matrix (scale divided out) as a quaternion
inline Quat rotationOf(const Mat4& m) {
    Vec3 c0{m.m[0], m.m[1], m.m[2]}, c1{m.m[4], m.m[5], m.m[6]}, c2{m.m[8], m.m[9], m.m[10]};
    float sx = length(c0), sy = length(c1), sz = length(c2);
    if (sx < 1e-12f || sy < 1e-12f || sz < 1e-12f) return {};
    float r00 = c0.x / sx, r10 = c0.y / sx, r20 = c0.z / sx, r01 = c1.x / sy, r11 = c1.y / sy, r21 = c1.z / sy;
    float r02 = c2.x / sz, r12 = c2.y / sz, r22 = c2.z / sz;
    float tr = r00 + r11 + r22;
    Quat q;
    if (tr > 0.f) {
        float s = std::sqrt(tr + 1.f) * 2.f;
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        float s = std::sqrt(1.f + r00 - r11 - r22) * 2.f;
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        float s = std::sqrt(1.f + r11 - r00 - r22) * 2.f;
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        float s = std::sqrt(1.f + r22 - r00 - r11) * 2.f;
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    return normalizeQuat(q);
}
