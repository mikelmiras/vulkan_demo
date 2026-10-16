#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Foliage: one terrain's scattered plants (terrain::scatterFoliage), drawn instanced with scene.frag. Each is turned
// around the vertical and scaled; its tips sway in the wind (more the higher the vertex); it shrinks away towards
// its fade distance.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out vec4 vColor;
layout(location = 4) flat out vec4 vParams;
layout(location = 5) flat out vec4 vMaterial;
layout(location = 6) flat out vec4 vEmissive;
layout(location = 7) out vec4 vSplat;
layout(location = 8) out float vHole;
layout(location = 9) flat out vec4 vStream;
layout(location = 10) flat out vec4 vLayers;
layout(location = 11) flat out vec4 vSurface;

// Terrain::Plant: position (terrain space), yaw, scale, shade
layout(std430, set = 0, binding = 16) readonly buffer Plants { float plants[]; };
layout(push_constant) uniform Foliage {
    int shadowLayer;   // (the shadow pass's)
    int pad0, pad1, pad2;
    mat4 model;        // the terrain's
    vec4 color;        // rgb linear
    vec4 params;       // x = wind, y = fade distance
} pc;

invariant gl_Position;   // (the depth prepass and the lit pass after it must land on the same depths)

void main() {
    uint i = uint(gl_InstanceIndex) * 6u;
    vec3 pos = vec3(plants[i], plants[i + 1u], plants[i + 2u]);
    float yaw = plants[i + 3u], scale = plants[i + 4u], shade = plants[i + 5u];
    vec4 base = pc.model * vec4(pos, 1.0);
    float dist = distance(base.xyz, frame.camPos.xyz);
    scale *= 1.0 - smoothstep(pc.params.y * 0.6, pc.params.y, dist);
    float c = cos(yaw), s = sin(yaw);
    mat3 turn = mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
    vec3 local = turn * (inPos * scale);
    // Wind: a slow sway and a quicker flutter, out of step from place to place, bending the tips
    float t = frame.time.x, bend = inPos.y * inPos.y * pc.params.x * 4.0;
    vec2 sway = vec2(sin(t * 1.3 + base.x * 0.21 + base.z * 0.17), 0.5 * sin(t * 2.9 + base.z * 0.31)) * bend;
    local.xz += sway * scale;
    vec4 world = pc.model * vec4(pos + local, 1.0);
    vWorldPos = world.xyz;
    vNormal = normalize(mat3(pc.model) * (turn * inNormal));
    vUV = inUV;
    vColor = vec4(pc.color.rgb * shade, 1.0);
    vParams = vec4(-1.0, 1.0, 0.0, -1.0);   // (x = -1: foliage, lit the same from both sides)
    vMaterial = vec4(0.0, 0.85, 0.0, 0.0);
    vEmissive = vec4(0.0);
    vStream = vec4(-1.0, 0.0, 0.0, 0.0);   // (grass isn't streamed)
    vLayers = vec4(0.0, 0.05, 0.0, 0.0);   // (no coat, sheen or anisotropy)
    vSurface = vec4(0.0, 8.0, 0.0, 0.0);   // (no parallax or detail; the weather wets it)
    vSplat = vec4(0.0);
    vHole = 0.0;
    gl_Position = frame.viewProj * world;
}
