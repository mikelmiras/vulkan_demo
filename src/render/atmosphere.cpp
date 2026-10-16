// The atmosphere's sums on the CPU (atmosphere.h): the same model and constants as shaders/atmosphere.glsl.
#include "render/atmosphere.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace atmosphere {

namespace {

// Per km at sea level: Rayleigh scattering (rgb), Mie scattering and extinction, ozone absorption; and how fast
// each thins with height (Hillaire's Earth)
const Vec3 kRayleigh{5.802e-3f, 13.558e-3f, 33.1e-3f};
constexpr float kMieScattering = 3.996e-3f, kMieExtinction = 4.44e-3f;
const Vec3 kOzone{0.65e-3f, 1.881e-3f, 0.085e-3f};
constexpr float kRayleighHeight = 8.f, kMieHeight = 1.2f, kMieG = 0.8f;


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

namespace {
// The transmittance and multiple-scattering tables on the CPU (the same as the GPU's, smaller), made once per haze
struct Tables {
    float haze = -1.f;
    static constexpr int kTmu = 64, kTh = 32, kMmu = 16, kMh = 16;
    std::vector<Vec3> trans, multiple;
};
Tables& tablesFor(float haze);

Vec3 lerp3(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
Vec3 sampleTable(const std::vector<Vec3>& t, int nx, int ny, float x, float y) {   // (x, y: 0..1, bilinear)
    float fx = std::clamp(x, 0.f, 1.f) * (nx - 1), fy = std::clamp(y, 0.f, 1.f) * (ny - 1);
    int x0 = std::min((int)fx, nx - 2), y0 = std::min((int)fy, ny - 2);
    float ax = fx - x0, ay = fy - y0;
    Vec3 a = lerp3(t[(size_t)(y0 * nx + x0)], t[(size_t)(y0 * nx + x0 + 1)], ax);
    Vec3 b = lerp3(t[(size_t)((y0 + 1) * nx + x0)], t[(size_t)((y0 + 1) * nx + x0 + 1)], ax);
    return lerp3(a, b, ay);
}
// (the tables' axes: cos from straight up, -1..1, and height, 0..the top)
Vec3 transmittanceFast(const Tables& t, float height, float mu) {
    const float r = kGroundRadius + std::max(height, 0.f);
    if (toSphere(r, mu, kGroundRadius) > 0.f) return {0, 0, 0};
    return sampleTable(t.trans, Tables::kTmu, Tables::kTh, mu * 0.5f + 0.5f, height / (kTopRadius - kGroundRadius));
}

// Hillaire's multiple scattering: at each height and sun angle, the light scattered twice reaching the point from
// all round (isotropically, 64 directions) over 1 - the fraction of it scattered again (an infinite series of
// orders: f + f^2 + ...); scattered towards the viewer by the medium's scattering, isotropically
Vec3 computeMultiple(const Tables& t, float height, float muSun, float haze) {
    const float r = kGroundRadius + std::max(height, 0.005f);   // (just off the ground: rays down meet it at once)
    const Vec3 toSun{std::sqrt(std::max(0.f, 1.f - muSun * muSun)), muSun, 0.f};
    constexpr int kDirs = 64, kSteps = 16;
    Vec3 second{0, 0, 0}, transfer{0, 0, 0};
    for (int i = 0; i < kDirs; ++i) {   // (a Fibonacci sphere)
        float y = 1.f - 2.f * (i + 0.5f) / kDirs, rr = std::sqrt(std::max(0.f, 1.f - y * y)), phi = i * 2.39996323f;
        Vec3 dir{rr * std::cos(phi), y, rr * std::sin(phi)};
        float ground = toSphere(r, dir.y, kGroundRadius);
        float length = ground > 0.f ? ground : toSphere(r, dir.y, kTopRadius);
        if (length <= 0.f) continue;
        Vec3 through{1, 1, 1};
        float dt = length / kSteps;
        for (int k = 0; k < kSteps; ++k) {
            Vec3 p{dir.x * (k + 0.5f) * dt, r + dir.y * (k + 0.5f) * dt, dir.z * (k + 0.5f) * dt};
            float pr = std::sqrt(dot(p, p));
            Medium m = mediumAt(pr - kGroundRadius, haze);
            Vec3 scatter = m.scatteringR + Vec3{1, 1, 1} * m.scatteringM;
            Vec3 stepThrough = expv(m.extinction * -dt);
            Vec3 integral{(1.f - stepThrough.x) / std::max(m.extinction.x, 1e-9f), (1.f - stepThrough.y) / std::max(m.extinction.y, 1e-9f),
                          (1.f - stepThrough.z) / std::max(m.extinction.z, 1e-9f)};
            Vec3 sun = transmittanceFast(t, pr - kGroundRadius, dot(p, toSun) / pr);
            second += mul(mul(mul(through, integral), scatter), sun) * (1.f / (4.f * PI));
            transfer += mul(mul(through, integral), scatter);
            through = mul(through, stepThrough);
        }
    }
    second = second * (4.f * PI / kDirs);
    transfer = transfer * (1.f / kDirs);   // (4 pi / N, times the isotropic phase 1 / 4 pi)
    Vec3 series{1.f / std::max(1.f - transfer.x, 1e-3f), 1.f / std::max(1.f - transfer.y, 1e-3f),
                1.f / std::max(1.f - transfer.z, 1e-3f)};
    return mul(second, series) * (1.f / (4.f * PI));   // (the isotropic phase towards the viewer, in advance)
}

std::mutex tablesMutex;
Tables& tablesFor(float haze) {
    static Tables tables;
    if (tables.haze == haze) return tables;
    tables.haze = haze;
    tables.trans.assign((size_t)Tables::kTmu * Tables::kTh, Vec3{});
    for (int y = 0; y < Tables::kTh; ++y)
        for (int x = 0; x < Tables::kTmu; ++x)
            tables.trans[(size_t)(y * Tables::kTmu + x)] =
                transmittance((float)y / (Tables::kTh - 1) * (kTopRadius - kGroundRadius), (float)x / (Tables::kTmu - 1) * 2.f - 1.f, haze);
    tables.multiple.assign((size_t)Tables::kMmu * Tables::kMh, Vec3{});
    for (int y = 0; y < Tables::kMh; ++y)
        for (int x = 0; x < Tables::kMmu; ++x)
            tables.multiple[(size_t)(y * Tables::kMmu + x)] = computeMultiple(
                tables, (float)y / (Tables::kMh - 1) * (kTopRadius - kGroundRadius), (float)x / (Tables::kMmu - 1) * 2.f - 1.f, haze);
    return tables;
}
}  // namespace

Vec3 multipleScattering(float height, float cosSunZenith, float haze) {
    std::lock_guard<std::mutex> lock(tablesMutex);
    const Tables& t = tablesFor(haze);
    return sampleTable(t.multiple, Tables::kMmu, Tables::kMh, cosSunZenith * 0.5f + 0.5f, height / (kTopRadius - kGroundRadius));
}

namespace {
Vec3 skyRadianceWith(const Tables& tables, Vec3 dir, Vec3 toSun, float haze);
}

Vec3 skyRadiance(Vec3 dir, Vec3 toSun, float haze) {
    std::lock_guard<std::mutex> lock(tablesMutex);
    return skyRadianceWith(tablesFor(haze), dir, toSun, haze);
}

namespace {
// (the sum along the view, with the tables' transmittance and multiple scattering: the GPU's sky_view.frag's)
Vec3 skyRadianceWith(const Tables& tables, Vec3 dir, Vec3 toSun, float haze) {
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
        Vec3 sun = transmittanceFast(tables, pr0 - kGroundRadius, dot(p, toSun) / pr0);
        // (and the light scattered more than once: the multiple-scattering table's, isotropic)
        Vec3 ms = sampleTable(tables.multiple, Tables::kMmu, Tables::kMh, dot(p, toSun) / pr0 * 0.5f + 0.5f,
                              (pr0 - kGroundRadius) / (kTopRadius - kGroundRadius));
        Vec3 scattered = mul(m.scatteringR * pr + Vec3{1, 1, 1} * (m.scatteringM * pm), sun) +
                         mul(m.scatteringR + Vec3{1, 1, 1} * m.scatteringM, ms);
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

}  // namespace

// The sky's radiance as spherical harmonics (the upper half: the ground is the caller's), for a sun of irradiance 1:
// 128 directions over the sphere (a Fibonacci spiral), each its radiance times the nine basis functions
void skyHarmonics(Vec3 toSun, float haze, Vec3 out[9]) {
    std::lock_guard<std::mutex> lock(tablesMutex);
    const Tables& tables = tablesFor(haze);
    for (int k = 0; k < 9; ++k) out[k] = {0, 0, 0};
    constexpr int kDirs = 128;
    for (int i = 0; i < kDirs; ++i) {
        float y = 1.f - 2.f * (i + 0.5f) / kDirs;
        if (y <= 0.f) continue;
        float rr = std::sqrt(std::max(0.f, 1.f - y * y)), phi = i * 2.39996323f;
        Vec3 d{rr * std::cos(phi), y, rr * std::sin(phi)};
        Vec3 L = skyRadianceWith(tables, d, toSun, haze);
        float basis[9];
        harmonics(d, basis);
        for (int k = 0; k < 9; ++k) out[k] += L * (basis[k] * 4.f * PI / kDirs);
    }
}

void harmonics(Vec3 d, float out[9]) {
    out[0] = 0.282095f;
    out[1] = 0.488603f * d.y;
    out[2] = 0.488603f * d.z;
    out[3] = 0.488603f * d.x;
    out[4] = 1.092548f * d.x * d.y;
    out[5] = 1.092548f * d.y * d.z;
    out[6] = 0.315392f * (3.f * d.z * d.z - 1.f);
    out[7] = 1.092548f * d.x * d.z;
    out[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

Vec3 sunDirection(float hours, float latitude, float turn) {
    const float h = (hours - 12.f) * (PI / 12.f), lat = latitude * DEG2RAD;
    Vec3 d{-std::sin(h), std::cos(lat) * std::cos(h), std::sin(lat) * std::cos(h)};
    float c = std::cos(turn), s = std::sin(turn);
    return normalize({c * d.x + s * d.z, d.y, -s * d.x + c * d.z});
}

void hemisphereHarmonics(bool upper, float out[9]) {
    // (exactly: over either half, every term but the constant and the vertical one sums to nothing)
    for (int k = 0; k < 9; ++k) out[k] = 0.f;
    out[0] = 0.282095f * 2.f * PI;
    out[1] = 0.488603f * PI * (upper ? 1.f : -1.f);
}

namespace {
// shaders/common.glsl's skyHash and cloudNoise (the same sums: the same clouds)
float fract(float x) { return x - std::floor(x); }
float skyHash(float px, float py, float pz) {
    px = fract(px * 0.1031f), py = fract(py * 0.1030f), pz = fract(pz * 0.0973f);
    float d = px * (py + 33.33f) + py * (px + 33.33f) + pz * (pz + 33.33f);
    px += d, py += d, pz += d;
    return fract((px + py) * pz);
}
float cloudNoise(float x, float y) {
    float ix = std::floor(x), iy = std::floor(y), fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3.f - 2.f * fx), uy = fy * fy * (3.f - 2.f * fy);
    float a = skyHash(ix, iy, 1.f), b = skyHash(ix + 1.f, iy, 1.f), c = skyHash(ix, iy + 1.f, 1.f), e = skyHash(ix + 1.f, iy + 1.f, 1.f);
    return (a + (b - a) * ux) * (1.f - uy) + (c + (e - c) * ux) * uy;
}
float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
}  // namespace

float cloudFraction(float cover) {
    cover = std::clamp(cover, 0.f, 1.f);
    if (cover <= 0.f) return 0.f;
    // The layer's density averaged over 48 x 48 points across 12 x 12 of its noise's cells (its pattern's scale)
    constexpr int kN = 48;
    double sum = 0.0;
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) {
            float x = (i + 0.37f) * (12.f / kN), y = (j + 0.61f) * (12.f / kN);
            float n = cloudNoise(x, y) * 0.6f + cloudNoise(x * 2.7f + 7.1f, y * 2.7f + 7.1f) * 0.3f +
                      cloudNoise(x * 7.3f + 3.3f, y * 7.3f + 3.3f) * 0.1f;
            sum += smoothstep(1.f - cover, 1.f - cover + 0.25f, n) * std::min(cover * 3.f, 1.f);
        }
    return (float)(sum / (kN * kN));
}

void cloudyHarmonics(Vec3 sh[9], float fraction, Vec3 cloud) {
    struct Upper {
        float k[9];
        Upper() { hemisphereHarmonics(true, k); }
    };
    static const Upper upperHalf;   // (made once)
    const float* upper = upperHalf.k;
    fraction = std::clamp(fraction, 0.f, 1.f);
    for (int k = 0; k < 9; ++k) sh[k] = sh[k] * (1.f - fraction) + cloud * (upper[k] * fraction);
}

bool skyChangedEnough(const SkyState& baked, const SkyState& now) {
    return dot(normalize(baked.toSun), normalize(now.toSun)) < std::cos(2.f * DEG2RAD) ||
           std::fabs(baked.cloudCover - now.cloudCover) > 0.05f ||
           std::fabs(baked.haze - now.haze) > 0.05f * std::max(baked.haze, 0.1f);
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
