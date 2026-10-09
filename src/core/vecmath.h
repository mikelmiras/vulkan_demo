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
