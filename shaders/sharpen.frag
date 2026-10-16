#version 450
// Contrast-adaptive sharpening (after AMD's FidelityFX CAS) of temporal AA's or the upscaler's result into the HDR
// picture: each pixel pushed away from its four neighbours by as much as their contrast leaves room for (less where
// it's already sharp or near the range's ends, so no halos); the history it came from stays unsharpened (no build-up
// frame after frame). Reads it through the other temporal set's history (renderer_taa.cpp).

layout(set = 0, binding = 1) uniform sampler2D picture;   // (this frame's temporal result)
layout(push_constant) uniform Push {
    mat4 unused;
    vec4 viewport;    // the scene's rectangle (pixels)
    vec4 params;      // x = sharpness (0..1), y, z = 1 / image size
    vec4 extra;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 compress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 expand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 centre = texelFetch(picture, p, 0);
    vec2 px = gl_FragCoord.xy;
    vec4 vp = pc.viewport;
    if (pc.params.x <= 0.0 || centre.a > 1.0 || px.x < vp.x || px.y < vp.y || px.x >= vp.x + vp.z || px.y >= vp.y + vp.w) {
        outColor = centre;   // (outside the scene, the outline's marker: as they are)
        return;
    }
    ivec2 hi = textureSize(picture, 0) - 1;
    vec3 e = compress(centre.rgb);
    vec3 b = compress(texelFetch(picture, clamp(p + ivec2(0, -1), ivec2(0), hi), 0).rgb);
    vec3 d = compress(texelFetch(picture, clamp(p + ivec2(-1, 0), ivec2(0), hi), 0).rgb);
    vec3 f = compress(texelFetch(picture, clamp(p + ivec2(1, 0), ivec2(0), hi), 0).rgb);
    vec3 h = compress(texelFetch(picture, clamp(p + ivec2(0, 1), ivec2(0), hi), 0).rgb);
    vec3 mn = min(e, min(min(b, d), min(f, h))), mx = max(e, max(max(b, d), max(f, h)));
    vec3 amount = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 w = amount * (-1.0 / mix(8.0, 5.0, clamp(pc.params.x, 0.0, 1.0)));
    vec3 sharp = clamp((b + d + f + h) * w + e, 0.0, 1.0) / (1.0 + 4.0 * w);
    outColor = vec4(expand(clamp(sharp, 0.0, 0.999)), centre.a);
}
