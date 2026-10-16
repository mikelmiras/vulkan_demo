#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Particles: a camera-facing quad per instance (six vertices, no vertex buffer): a round dot, or a streak from the
// particle back along its motion (rain), facing the camera across its length

struct Particle {
    vec4 posSize;   // xyz = centre (a streak's head), w = size (metres across)
    vec4 color;     // rgb linear (times brightness), a = opacity
    vec4 streak;    // xyz: back to its tail (0: a dot), w = 1: lit by the scene's lights
};
layout(std430, set = 0, binding = 4) readonly buffer Particles {
    Particle particles[];
};

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vWorld;
layout(location = 3) flat out vec2 vKind;   // x = 1: a streak, y = 1: lit

const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    Particle p = particles[gl_InstanceIndex];
    vec2 c = corners[gl_VertexIndex];
    vec3 world;
    float len = length(p.streak.xyz);
    if (len > 1e-4) {   // (from the head, c.y = 1, to the tail, c.y = -1; across, facing the camera)
        vec3 along = p.streak.xyz / len;
        vec3 across = cross(along, normalize(p.posSize.xyz - frame.camPos.xyz));
        across = dot(across, across) > 1e-8 ? normalize(across) : frame.camRight.xyz;
        world = p.posSize.xyz - p.streak.xyz * (0.5 - 0.5 * c.y) + across * (c.x * p.posSize.w * 0.5);
    } else {
        world = p.posSize.xyz + (frame.camRight.xyz * c.x + frame.camUp.xyz * c.y) * (p.posSize.w * 0.5);
    }
    vCorner = c;
    vColor = p.color;
    vWorld = world;
    vKind = vec2(len > 1e-4 ? 1.0 : 0.0, p.streak.w);
    gl_Position = frame.viewProj * vec4(world, 1.0);
}
