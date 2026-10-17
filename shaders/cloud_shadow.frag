#version 450
// The volumetric clouds' shadow (renderer_trace.cpp's recordCloudShadow, before the scene pass, when they're marched):
// for each texel of a square around the camera, at height 0, the sunlight through the cloud slab: the same clouds
// (clouds.glsl's cloudAt, eroded) sampled along the sun's ray across it, Beer's law. scene.frag reads it (binding 46)
// where the ray from a point at any height meets the plane, so the ground's shadows have the shapes in the sky.
// render/cloud_shape.cpp's shadowUv and shadowThrough: the same sums.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "clouds.glsl"

layout(location = 0) out float outThrough;

const float kShadowTexels = 256.0;   // (renderer.h's kCloudShadowSize)

void main() {
    vec2 xz = frame.cloudShadowArea.xy + gl_FragCoord.xy / kShadowTexels * frame.cloudShadowArea.z;
    vec3 L = normalize(frame.skyToSun.xyz);
    if (L.y <= 0.02 || frame.atmosphere.z > 0.5) {   // (the sun down, or the moon: no shadows of theirs)
        outThrough = 1.0;
        return;
    }
    // Across the slab along the ray: eight samples, a little dithered
    float t0 = kCloudBase / L.y, t1 = kCloudTop / L.y, ds = (t1 - t0) / 8.0;
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float depth = 0.0;
    for (int i = 0; i < 8; ++i) depth += cloudAt(vec3(xz, 0.0) + L * (t0 + (float(i) + jitter) * ds), true);
    // (one and a half times the view's extinction: a cloud lets through less than its edge looks; none below a
    // quarter, as the flat layer's)
    outThrough = max(exp(-depth * ds * kCloudExtinction * 1.5), 0.25);
}
