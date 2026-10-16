#version 450
// The physically based sky's transmittance table (atmosphere.glsl): for each height and direction, how much light
// gets through the air to the top of the atmosphere. Made again only when the haze changes.
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"

layout(push_constant) uniform Push {
    vec4 a;   // x = haze, yz = 1 / the table's size
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    float r, mu;
    transmittanceParams(gl_FragCoord.xy * pc.a.yz, r, mu);
    float dist = toSphere(r, mu, kTopRadius);
    const int kSteps = 40;
    vec3 depth = vec3(0.0);
    for (int i = 0; i < kSteps; ++i) {
        float t = (float(i) + 0.5) / float(kSteps) * dist;
        float h = sqrt(r * r + t * t + 2.0 * r * t * mu) - kGroundRadius;
        depth += mediumAt(h, pc.a.x).extinction * (dist / float(kSteps));
    }
    outColor = vec4(exp(-depth), 1.0);
}
