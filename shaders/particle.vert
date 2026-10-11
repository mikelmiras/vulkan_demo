#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Particles: a camera-facing quad per instance (six vertices, no vertex buffer)

struct Particle {
    vec4 posSize;   // xyz = centre, w = size (metres across)
    vec4 color;     // rgb linear (times brightness), a = opacity
};
layout(std430, set = 0, binding = 4) readonly buffer Particles {
    Particle particles[];
};

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;

const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    Particle p = particles[gl_InstanceIndex];
    vec2 c = corners[gl_VertexIndex];
    vec3 world = p.posSize.xyz + (frame.camRight.xyz * c.x + frame.camUp.xyz * c.y) * (p.posSize.w * 0.5);
    vCorner = c;
    vColor = p.color;
    gl_Position = frame.viewProj * vec4(world, 1.0);
}
