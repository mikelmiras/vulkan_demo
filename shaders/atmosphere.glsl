// The physically based sky's model (src/render/atmosphere.h has the same on the CPU): Earth's atmosphere as Rayleigh
// scattering (air), Mie scattering (haze) and ozone absorption, in km. Shared by the passes that make its look-up
// tables (renderer_sky.cpp): how much light gets through the air (transmittance), and the sky around the viewer.

const float kGroundRadius = 6360.0, kTopRadius = 6460.0, kViewHeight = 0.2;
const vec3 kRayleigh = vec3(5.802e-3, 13.558e-3, 33.1e-3);
const float kMieScattering = 3.996e-3, kMieExtinction = 4.44e-3;
const vec3 kOzone = vec3(0.65e-3, 1.881e-3, 0.085e-3);
const float kRayleighHeight = 8.0, kMieHeight = 1.2, kMieG = 0.8;
const float kAtmPi = 3.14159265;
const float kMultiple = 0.5 / (4.0 * kAtmPi);   // (multiple scattering, roughly: an extra isotropic phase)

struct Medium {
    vec3 scatteringR;
    float scatteringM;
    vec3 extinction;
};
Medium mediumAt(float height, float haze) {
    float r = exp(-height / kRayleighHeight), m = exp(-height / kMieHeight) * haze;
    float ozone = max(0.0, 1.0 - abs(height - 25.0) / 15.0);
    Medium md;
    md.scatteringR = kRayleigh * r;
    md.scatteringM = kMieScattering * m;
    md.extinction = md.scatteringR + vec3(kMieExtinction * m) + kOzone * ozone;
    return md;
}

// The distance along a ray from radius r (cosine mu from straight up) to a sphere: -1 if it misses
float toSphere(float r, float mu, float radius) {
    float b = r * mu, c = r * r - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t = -b - s;
    return t > 0.0 ? t : (-b + s > 0.0 ? -b + s : -1.0);
}

float rayleighPhase(float c) { return 3.0 / (16.0 * kAtmPi) * (1.0 + c * c); }
float miePhase(float c) {   // (Cornette-Shanks)
    float g2 = kMieG * kMieG;
    float d = 1.0 + g2 - 2.0 * kMieG * c;
    return 3.0 / (8.0 * kAtmPi) * (1.0 - g2) * (1.0 + c * c) / ((2.0 + g2) * d * sqrt(d));
}

// The transmittance table: x the direction (from straight up to grazing the ground), y the height; Bruneton's
// mapping (texels spent where the light changes)
const float kAtmH = sqrt(kTopRadius * kTopRadius - kGroundRadius * kGroundRadius);
vec2 transmittanceUv(float r, float mu) {
    float rho = sqrt(max(r * r - kGroundRadius * kGroundRadius, 0.0));
    float d = max(-r * mu + sqrt(max(r * r * (mu * mu - 1.0) + kTopRadius * kTopRadius, 0.0)), 0.0);
    float dMin = kTopRadius - r, dMax = rho + kAtmH;
    return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / kAtmH);
}
void transmittanceParams(vec2 uv, out float r, out float mu) {
    float rho = kAtmH * uv.y;
    r = sqrt(rho * rho + kGroundRadius * kGroundRadius);
    float dMin = kTopRadius - r, dMax = rho + kAtmH;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d <= 0.0 ? 1.0 : clamp((kAtmH * kAtmH - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}
// The light left reaching radius r from the sun along mu (none where the planet is in the way)
vec3 sunTransmittance(sampler2D table, float r, float mu) {
    if (toSphere(r, mu, kGroundRadius) > 0.0) return vec3(0.0);
    return textureLod(table, transmittanceUv(r, mu), 0.0).rgb;
}
