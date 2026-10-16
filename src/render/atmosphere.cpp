// The atmosphere's sums on the CPU (atmosphere.h): the same model and constants as shaders/atmosphere.glsl.
#include "render/atmosphere.h"

#include <algorithm>
#include <cmath>

namespace atmosphere {

namespace {

// Per km at sea level: Rayleigh scattering (rgb), Mie scattering and extinction, ozone absorption; and how fast
// each thins with height (Hillaire's Earth)
const Vec3 kRayleigh{5.802e-3f, 13.558e-3f, 33.1e-3f};
constexpr float kMieScattering = 3.996e-3f, kMieExtinction = 4.44e-3f;
const Vec3 kOzone{0.65e-3f, 1.881e-3f, 0.085e-3f};
constexpr float kRayleighHeight = 8.f, kMieHeight = 1.2f, kMieG = 0.8f;
// Multiple scattering, roughly: an extra isotropic phase (1 / 4 pi) times this
constexpr float kMultiple = 0.5f / (4.f * PI);

Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 expv(Vec3 a) { return {std::exp(a.x), std::exp(a.y), std::exp(a.z)}; }

struct Medium {
    Vec3 scatteringR;     // Rayleigh's (its extinction: the same)
    float scatteringM;    // Mie's
    Vec3 extinction;      // everything's
};
Medium mediumAt(float height, float haze) {
    float r = std::exp(-height / kRayleighHeight), m = std::exp(-height / kMieHeight) * haze;
    float ozone = std::max(0.f, 1.f - std::fabs(height - 25.f) / 15.f);
    Medium md;
    md.scatteringR = kRayleigh * r;
    md.scatteringM = kMieScattering * m;
    md.extinction = md.scatteringR + Vec3{1, 1, 1} * (kMieExtinction * m) + kOzone * ozone;
    return md;
}

// The distance along a ray from radius r (cosine mu from straight up) to a sphere of `radius`: -1 if it misses
float toSphere(float r, float mu, float radius) {
    float b = r * mu, c = r * r - radius * radius;
    float disc = b * b - c;
    if (disc < 0.f) return -1.f;
    float s = std::sqrt(disc);
    float t = -b - s;
    return t > 0.f ? t : -b + s > 0.f ? -b + s : -1.f;
}

float rayleighPhase(float c) { return 3.f / (16.f * PI) * (1.f + c * c); }
float miePhase(float c) {   // (Cornette-Shanks)
    float g2 = kMieG * kMieG;
    float d = 1.f + g2 - 2.f * kMieG * c;
    return 3.f / (8.f * PI) * (1.f - g2) * (1.f + c * c) / ((2.f + g2) * d * std::sqrt(d));
}

}  // namespace

Vec3 transmittance(float height, float cosZenith, float haze) {
    const float r = kGroundRadius + std::max(height, 0.f);
    if (toSphere(r, cosZenith, kGroundRadius) > 0.f) return {0, 0, 0};   // (into the ground)
    const float length = toSphere(r, cosZenith, kTopRadius);
    if (length <= 0.f) return {1, 1, 1};
    constexpr int kSteps = 40;
    Vec3 depth{0, 0, 0};
    for (int i = 0; i < kSteps; ++i) {
        float t = (i + 0.5f) / kSteps * length;
        float h = std::sqrt(r * r + t * t + 2.f * r * t * cosZenith) - kGroundRadius;
        depth += mediumAt(h, haze).extinction * (length / kSteps);
    }
    return expv(depth * -1.f);
}

Vec3 skyRadiance(Vec3 dir, Vec3 toSun, float haze) {
    const float r = kGroundRadius + kViewHeight;
    float ground = toSphere(r, dir.y, kGroundRadius);
    float length = ground > 0.f ? ground : toSphere(r, dir.y, kTopRadius);
    if (length <= 0.f) return {0, 0, 0};
    const float c = dot(dir, toSun);
    const float pr = rayleighPhase(c), pm = miePhase(c);
    constexpr int kSteps = 30;
    Vec3 light{0, 0, 0}, through{1, 1, 1};
    for (int i = 0; i < kSteps; ++i) {
        float dt = length / kSteps, t = (i + 0.5f) * dt;
        // (the point's position, from the planet's centre: up is +y)
        Vec3 p{dir.x * t, r + dir.y * t, dir.z * t};
        float pr0 = std::sqrt(dot(p, p));
        Medium m = mediumAt(pr0 - kGroundRadius, haze);
        Vec3 sun = transmittance(pr0 - kGroundRadius, dot(p, toSun) / pr0, haze);
        // (light scattered more than once: approximated as the same sunlight scattered again alike every way)
        Vec3 scattered = mul(m.scatteringR * (pr + kMultiple) + Vec3{1, 1, 1} * (m.scatteringM * (pm + kMultiple)), sun);
        // (each step's light, integrated through the step's own extinction: Hillaire's energy-conserving form)
        Vec3 stepThrough = expv(m.extinction * -dt);
        Vec3 integral{(scattered.x - scattered.x * stepThrough.x) / std::max(m.extinction.x, 1e-9f),
                      (scattered.y - scattered.y * stepThrough.y) / std::max(m.extinction.y, 1e-9f),
                      (scattered.z - scattered.z * stepThrough.z) / std::max(m.extinction.z, 1e-9f)};
        light += mul(through, integral);
        through = mul(through, stepThrough);
    }
    return light;
}

Vec3 sunDirection(float hours, float latitude, float turn) {
    const float h = (hours - 12.f) * (PI / 12.f), lat = latitude * DEG2RAD;
    Vec3 d{-std::sin(h), std::cos(lat) * std::cos(h), std::sin(lat) * std::cos(h)};
    float c = std::cos(turn), s = std::sin(turn);
    return normalize({c * d.x + s * d.z, d.y, -s * d.x + c * d.z});
}

Vec3 seaLevelExtinction(float haze) { return mediumAt(0.f, haze).extinction; }

SkyLight skyLight(Vec3 toSun, float haze) {
    SkyLight l;
    l.sun = transmittance(kViewHeight, toSun.y, haze);
    l.zenith = skyRadiance({0, 1, 0}, toSun, haze);
    l.horizon = {0, 0, 0};
    constexpr int kAround = 8;
    const float e = 3.f * DEG2RAD;
    for (int i = 0; i < kAround; ++i) {
        float a = (i + 0.5f) * (2.f * PI / kAround);
        l.horizon += skyRadiance({std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)}, toSun, haze) *
                     (1.f / kAround);
    }
    return l;
}

}  // namespace atmosphere
