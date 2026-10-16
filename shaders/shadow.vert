#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Shadow maps: depth only, from a light's point of view (with what shadow_cutout.frag needs for casters with holes)

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out vec4 vCutout;   // x = opacity, y = textured, z = alpha mode, w = cutoff
layout(location = 2) flat out vec4 vTint;     // rgb = colour (linear), w = translucency (shadow_tint.frag)

layout(push_constant) uniform Shadow {
    int layer;   // which shadow camera (frame.shadowViewProj)
} shadow;

void main() {
    Instance inst = instances[instanceIndex[gl_InstanceIndex]];
    vUV = inUV * inst.params.y;
    vCutout = vec4(inst.color.a, inst.params.z, inst.material.w, inst.emissive.w);
    vTint = vec4(inst.color.rgb, inst.scatter.x);
    gl_Position = frame.shadowViewProj[shadow.layer] * SKINNED_MODEL(inst) * vec4(inPos, 1.0);
}
