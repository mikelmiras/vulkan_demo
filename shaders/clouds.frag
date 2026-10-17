#version 450
// The main view's volumetric clouds at half size (renderer_trace.cpp's recordClouds, before the scene pass): each
// half-size pixel's direction marched (clouds.glsl), then blended with what last frame found that way (the clouds are
// kilometres off, so only the camera's turn moves them on screen: the direction through last frame's camera finds
// them), so each frame's noisy march is a fifth of what shows. sky.frag brings them up (binding 44).
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "clouds.glsl"

layout(set = 0, binding = 45) uniform sampler2D cloudHistory;   // last frame's (the other frame's image)

layout(location = 0) out vec4 outClouds;

void main() {
    vec2 area = ceil(frame.drawnViewport.zw * 0.5);   // (the half-size pixels covering the drawn picture)
    vec2 ndc = gl_FragCoord.xy / area * 2.0 - 1.0;
    float tanY = frame.camForward.w, aspect = frame.viewport.x / max(frame.viewport.y, 1.0);
    vec3 dir = normalize(frame.camForward.xyz + frame.camRight.xyz * (ndc.x * tanY * aspect) -
                         frame.camUp.xyz * (ndc.y * tanY));   // (as sky.frag's)
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + mod(frame.time.y, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
    vec4 clouds = volumetricClouds(dir, noise);
    if (frame.skyToSun.w > 1.5) {   // (a history: last frame marched them too)
        vec4 prev = frame.motionPrevViewProj * vec4(dir, 0.0);   // (a direction: the camera's move doesn't count)
        vec2 uv = prev.xy / max(prev.w, 1e-6) * 0.5 + 0.5;
        if (prev.w > 1e-6 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))))
            clouds = mix(textureLod(cloudHistory, uv * area / vec2(textureSize(cloudHistory, 0)), 0.0), clouds, 0.2);
    }
    outClouds = clouds;
}
