#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main() {
    float tanY = frame.camForward.w, aspect = frame.viewport.x / max(frame.viewport.y, 1.0);
    vec3 dir = normalize(frame.camForward.xyz + frame.camRight.xyz * (vNdc.x * tanY * aspect) -
                         frame.camUp.xyz * (vNdc.y * tanY));   // (clip-space y points down)
    outColor = vec4(skyColor(dir, true), 1.0);
}
