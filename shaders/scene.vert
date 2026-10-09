#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = transpose(inverse(mat3(pc.model))) * inNormal;
    vUV = inUV * pc.params.y;
    gl_Position = frame.viewProj * world;
}
