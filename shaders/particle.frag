#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// A soft round dot. Additive particles output light (blended ONE, ONE), others colour over what's behind.

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    float d = length(vCorner);
    if (d >= 1.0) discard;
    float a = vColor.a * (1.0 - smoothstep(0.35, 1.0, d));
    outColor = vec4(vColor.rgb * a, a);   // premultiplied: the blend states take it from here
}
