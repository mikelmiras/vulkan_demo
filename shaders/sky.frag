#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "fog.glsl"

layout(set = 0, binding = 21) uniform sampler3D fogVolume;   // volumetric fog (fog.glsl): the sky is behind all of it

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main() {
    float tanY = frame.camForward.w, aspect = frame.viewport.x / max(frame.viewport.y, 1.0);
    vec3 dir = normalize(frame.camForward.xyz + frame.camRight.xyz * (vNdc.x * tanY * aspect) -
                         frame.camUp.xyz * (vNdc.y * tanY));   // (clip-space y points down)
    vec3 color = skyColor(dir, true);
    if (frame.fogParams.x > 0.5) {
        vec4 f = texture(fogVolume, vec3(vNdc * 0.5 + 0.5, 1.0));
        color = color * f.a + f.rgb;
    }
    outColor = vec4(color, 1.0);
}
