#version 450
// Screen-space GI's bounce denoised (renderer_trace.cpp, after gi_trace.frag, at half size): an edge-aware blur of
// this frame's traced light (its history blended in already), 3 x 3 taps two texels apart, each weighed by a
// Gaussian, by how near its distance is to this one's and by how alike its normal is, so the grain of rays meeting
// things sporadically evens out without the light bleeding across edges or onto other surfaces. scene.frag brings
// the result up (binding 32); the history keeps the raw trace (binding 34), so the blur doesn't compound. The
// weights: render/gi_denoise.cpp's tapWeight, the same sums.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 30) uniform sampler2D sceneSurface;   // full size: xy = normal (octahedral), w >= 1: drawn
layout(set = 0, binding = 35) uniform sampler2D tracedDepth;    // half size: r = the traced pixel's distance
layout(set = 0, binding = 43) uniform sampler2D rawLight;       // half size: this frame's traced light

layout(location = 0) out vec4 outLight;

vec3 fromOctahedral(vec2 p) {
    vec3 n = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(rawLight, 0), full = textureSize(sceneSurface, 0);
    int k = int(frame.trace.z + 0.5);   // (the full-size pixel each half-size one traced this frame)
    ivec2 offset = ivec2(k & 1, k >> 1);
    vec4 centre = texelFetch(rawLight, p, 0);
    float z = texelFetch(tracedDepth, p, 0).r;
    if (z > 5000.0) {   // (the sky: nothing to blur)
        outLight = centre;
        return;
    }
    vec3 n = fromOctahedral(texelFetch(sceneSurface, min(p * 2 + offset, full - 1), 0).xy);
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 q = clamp(p + ivec2(x, y) * 2, ivec2(0), size - 1);
            float qz = texelFetch(tracedDepth, q, 0).r;
            if (qz > 5000.0) continue;
            vec3 qn = fromOctahedral(texelFetch(sceneSurface, min(q * 2 + offset, full - 1), 0).xy);
            float w = (x == 0 ? 1.0 : 0.5) * (y == 0 ? 1.0 : 0.5);
            w *= exp(-abs(qz - z) / (0.03 * z + 0.02));        // (the same surface's depth, about)
            w *= pow(max(dot(qn, n), 0.0), 8.0);              // (facing the same way)
            sum += texelFetch(rawLight, q, 0) * w;
            total += w;
        }
    outLight = total > 1e-4 ? sum / total : centre;
}
