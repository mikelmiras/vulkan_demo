#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor; // rgb = fog/sky colour, w = density
    vec4 viewport; // width, height, outline width (px)
} frame;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color; // rgb = albedo, a = 0 for checkered ground
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = transpose(inverse(mat3(pc.model))) * inNormal;
    gl_Position = frame.viewProj * world;
}
