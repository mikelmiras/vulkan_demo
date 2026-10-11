#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Shadow map: depth only, from the directional light's point of view

layout(location = 0) in vec3 inPos;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

layout(push_constant) uniform Shadow {
    int layer;   // which shadow camera (frame.shadowViewProj)
} shadow;

void main() {
    gl_Position = frame.shadowViewProj[shadow.layer] * SKINNED_MODEL(instances[gl_InstanceIndex]) * vec4(inPos, 1.0);
}
