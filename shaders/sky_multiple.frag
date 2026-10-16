#version 450
// The physically based sky's multiple-scattering table (atmosphere.glsl; Hillaire 2020): for each height (y) and
// sun angle (x: its cos from straight up, -1..1), the light scattered twice reaching the point from all round (64
// directions, isotropically), over 1 - the fraction scattered again (all further orders as a series), already
// divided by 4 pi: times the medium's scattering, the light it sends the viewer. Made again with the transmittance.
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"

layout(set = 0, binding = 0) uniform sampler2D transmittanceTable;
layout(push_constant) uniform Push {
    vec4 a;   // x = haze, yz = 1 / the table's size
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5) * pc.a.yz / max(vec2(1.0) - pc.a.yz, vec2(1e-4));   // (0..1 at the texels' centres)
    float muSun = uv.x * 2.0 - 1.0;
    float r = kGroundRadius + max(uv.y * (kTopRadius - kGroundRadius), 0.005);   // (just off the ground)
    vec3 toSun = vec3(sqrt(max(0.0, 1.0 - muSun * muSun)), muSun, 0.0);
    const int kDirs = 64, kSteps = 20;
    vec3 second = vec3(0.0), transfer = vec3(0.0);
    for (int i = 0; i < kDirs; ++i) {   // (a Fibonacci sphere)
        float y = 1.0 - 2.0 * (float(i) + 0.5) / float(kDirs), rr = sqrt(max(0.0, 1.0 - y * y)), phi = float(i) * 2.39996323;
        vec3 dir = vec3(rr * cos(phi), y, rr * sin(phi));
        float ground = toSphere(r, dir.y, kGroundRadius);
        float dist = ground > 0.0 ? ground : toSphere(r, dir.y, kTopRadius);
        if (dist <= 0.0) continue;
        vec3 through = vec3(1.0);
        float dt = dist / float(kSteps);
        for (int k = 0; k < kSteps; ++k) {
            vec3 p = vec3(dir.x, 0.0, dir.z) * ((float(k) + 0.5) * dt) + vec3(0.0, r + dir.y * (float(k) + 0.5) * dt, 0.0);
            float pr = length(p);
            Medium m = mediumAt(pr - kGroundRadius, pc.a.x);
            vec3 scatter = m.scatteringR + vec3(m.scatteringM);
            vec3 stepThrough = exp(-m.extinction * dt);
            vec3 integral = (vec3(1.0) - stepThrough) / max(m.extinction, vec3(1e-9));
            vec3 sun = sunTransmittance(transmittanceTable, pr, dot(p, toSun) / pr);
            second += through * integral * scatter * sun / (4.0 * kAtmPi);
            transfer += through * integral * scatter;
            through *= stepThrough;
        }
    }
    second *= 4.0 * kAtmPi / float(kDirs);
    transfer /= float(kDirs);
    outColor = vec4(second / max(vec3(1.0) - transfer, vec3(1e-3)) / (4.0 * kAtmPi), 1.0);
}
