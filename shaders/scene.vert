#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out vec4 vColor;
layout(location = 4) flat out vec4 vParams;
layout(location = 5) flat out vec4 vMaterial;
layout(location = 6) flat out vec4 vEmissive;

void main() {
    Instance inst = instances[gl_InstanceIndex];
    mat4 model = SKINNED_MODEL(inst);
    vec4 world = model * vec4(inPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = transpose(inverse(mat3(model))) * inNormal;
    vUV = inUV * inst.params.y;
    vColor = inst.color;
    vParams = inst.params;
    vMaterial = inst.material;
    vEmissive = inst.emissive;
    gl_Position = frame.viewProj * world;
}
