#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "water.glsl"

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
layout(location = 7) out vec4 vSplat;   // terrains: how much of each layer here (their skin weights)
layout(location = 8) out float vHole;   // terrains: > 0 on triangles using a hole point (their first joint)
layout(location = 9) flat out vec4 vStream;   // texture streaming (Instance.stream)
layout(location = 10) flat out vec4 vLayers;  // clear coat, its roughness, sheen, anisotropy (Instance.layers)

invariant gl_Position;   // (the depth prepass and the lit pass after it must land on the same depths)

void main() {
    Instance inst = instances[instanceIndex[gl_InstanceIndex]];
    mat4 model = SKINNED_MODEL(inst);
    vec4 world = model * vec4(inPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = transpose(inverse(mat3(model))) * inNormal;
    vUV = inUV * inst.params.y;
    vColor = inst.color;
    vParams = inst.params;
    vMaterial = inst.material;
    vEmissive = inst.emissive;
    vStream = inst.stream;
    vLayers = inst.layers;
    vSplat = inWeights;
    vHole = inst.params.x > 1.5 && inst.params.x < 2.5 ? float(inJoints.x) : 0.0;
    if (inst.params.x > 2.5) {   // water: its grid moved by the waves (rest positions in the world), how high a crest is
        vec3 offset, normal;
        gerstner(world.xz, inst.stream.y, inst.emissive, offset, normal);
        world.xyz += offset;
        vWorldPos = world.xyz;
        vNormal = normal;
        vSplat = vec4(offset.y / max(inst.emissive.x * 1.65, 1e-3), 0.0, 0.0, 0.0);   // (1: the highest a crest goes)
    }
    gl_Position = frame.viewProj * world;
}
