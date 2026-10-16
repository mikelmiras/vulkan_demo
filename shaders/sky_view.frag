#version 450
// The physically based sky around the viewer (atmosphere.glsl), for a sun of irradiance 1: light scattered towards
// the viewer along each direction, marched through the air with the sun's light at each step from the transmittance
// table. x: the angle from the sun around the vertical, y: the elevation (texels spent near the sun and the horizon:
// skyViewUv in common.glsl reads it). Made again when the sun moves.
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"

layout(set = 0, binding = 0) uniform sampler2D transmittanceTable;
layout(push_constant) uniform Push {
    vec4 a;   // xyz = towards the sun, w = haze
    vec4 b;   // xy = 1 / the table's size
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy * pc.b.xy;
    float elev = uv.y < 0.5 ? 0.5 * kAtmPi * (1.0 - 2.0 * uv.y) * (1.0 - 2.0 * uv.y)
                            : -0.5 * kAtmPi * (2.0 * uv.y - 1.0) * (2.0 * uv.y - 1.0);
    float cosAz = 1.0 - 2.0 * uv.x * uv.x, sinAz = sqrt(max(1.0 - cosAz * cosAz, 0.0));
    vec3 toSun = normalize(pc.a.xyz);
    vec2 sunH = length(toSun.xz) > 1e-4 ? normalize(toSun.xz) : vec2(1.0, 0.0);
    vec3 dir = vec3(cos(elev) * (cosAz * sunH.x - sinAz * sunH.y), sin(elev), cos(elev) * (cosAz * sunH.y + sinAz * sunH.x));

    const float r = kGroundRadius + kViewHeight;
    float ground = toSphere(r, dir.y, kGroundRadius);
    float dist = ground > 0.0 ? ground : toSphere(r, dir.y, kTopRadius);
    float c = dot(dir, toSun);
    float pr = rayleighPhase(c) + kMultiple, pm = miePhase(c) + kMultiple;
    const int kSteps = 30;
    vec3 light = vec3(0.0), through = vec3(1.0);
    float dt = max(dist, 0.0) / float(kSteps);
    for (int i = 0; i < kSteps; ++i) {
        float t = (float(i) + 0.5) * dt;
        vec3 p = vec3(dir.x * t, r + dir.y * t, dir.z * t);
        float pr0 = sqrt(dot(p, p));
        Medium m = mediumAt(pr0 - kGroundRadius, pc.a.w);
        vec3 sun = sunTransmittance(transmittanceTable, pr0, dot(p, toSun) / pr0);
        vec3 scattered = (m.scatteringR * pr + vec3(m.scatteringM * pm)) * sun;
        vec3 stepThrough = exp(-m.extinction * dt);   // (integrated through the step: Hillaire's energy-conserving form)
        light += through * (scattered - scattered * stepThrough) / max(m.extinction, vec3(1e-9));
        through *= stepThrough;
    }
    outColor = vec4(light, 1.0);
}
