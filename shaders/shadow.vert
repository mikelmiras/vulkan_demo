#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Shadow map: depth only, from the directional light's point of view

layout(location = 0) in vec3 inPos;

void main() {
    gl_Position = frame.lightViewProj * pc.model * vec4(inPos, 1.0);
}
