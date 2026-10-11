#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Selection outline: push each vertex outwards along its normal by a fixed number of pixels.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 3) flat out vec4 vColor;

void main() {
    Instance inst = instances[gl_InstanceIndex];
    vColor = inst.color;
    vec4 world = inst.model * vec4(inPos, 1.0);
    vec3 normal = normalize(transpose(inverse(mat3(inst.model))) * inNormal);
    vec4 clip = frame.viewProj * world;
    vec4 clipN = frame.viewProj * vec4(normal, 0.0);

    // Screen-space direction of the normal (derivative of the perspective divide)
    vec2 dir = clipN.xy * clip.w - clip.xy * clipN.w;
    if (dot(dir, dir) > 1e-12) dir = normalize(dir);

    clip.xy += dir * (2.0 * frame.viewport.z / frame.viewport.xy) * clip.w;
    gl_Position = clip;
}
