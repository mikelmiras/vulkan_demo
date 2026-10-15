#version 450
// Temporal anti-aliasing: this frame (drawn with a sub-pixel jitter) blended with the frames before it. The history
// is looked up where this pixel's point was last frame (reprojected through the depth) and clamped to the colours
// around this pixel now, so it can't drag old colours along (ghosting) when things move.
//
// With a render scale under 1 the scene was drawn smaller, into the corner of the viewport (params.w of its width and
// height): this frame's picture is read there (where this full-size pixel falls in it, un-jittered) and the history
// is kept at full size, so the frames' different jitters fill in the detail (temporal upscaling). Without history
// (temporal AA off: upscaling alone) it's a bilinear upscale, sharpened.

layout(set = 0, binding = 0) uniform sampler2D current;
layout(set = 0, binding = 1) uniform sampler2D history;
#ifndef UPSCALE_ONLY
layout(set = 0, binding = 2) uniform sampler2D depthMap;
#endif
layout(push_constant) uniform Push {
    mat4 reproject;   // this frame's clip space -> last frame's
    vec4 viewport;    // the scene's rectangle in the image (pixels: full size)
    vec4 params;      // x = how much history (0: none yet), y, z = 1 / image size, w = render scale
    vec4 extra;       // xy = this frame's jitter (drawn pixels), z = sharpening (0: none)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Blended in a compressed range: one very bright pixel doesn't outweigh the rest (and flicker)
vec3 compress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 expand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

void main() {
    vec2 px = gl_FragCoord.xy;
    vec4 vp = pc.viewport;
    bool inView = px.x >= vp.x && px.y >= vp.y && px.x < vp.x + vp.z && px.y < vp.y + vp.w;
    float scale = pc.params.w;
    bool scaled = scale < 0.999;
    if (!inView) {
        outColor = texture(current, vUV);
        return;
    }
    // Where this pixel is in the picture drawn (the same pixel at full scale; else smaller, un-jittered)
    vec2 drawn = scaled ? vp.xy + (px - vp.xy) * scale + pc.extra.xy : px;
    vec2 at = drawn * pc.params.yz;
    vec4 cur = scaled ? texture(current, at) : texture(current, vUV);
    if (pc.params.x <= 0.0 && !scaled) {
        outColor = cur;
        return;
    }
    // The colours around this pixel now (in the picture drawn), and their mean for sharpening
    vec3 lo = vec3(1e9), hi = vec3(-1e9), plus = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 c = compress(texture(current, at + vec2(x, y) * pc.params.yz).rgb);
            lo = min(lo, c);
            hi = max(hi, c);
            if (abs(x) + abs(y) == 1) plus += c;
        }
    vec3 now = compress(cur.rgb);
    if (pc.extra.z > 0.0) now = clamp(now + (now - plus * 0.25) * pc.extra.z, lo, hi);   // (sharpened, no halos)
#ifdef UPSCALE_ONLY
    outColor = vec4(expand(now), cur.a);   // (upscaling alone)
#else
    if (pc.params.x <= 0.0) {   // (no history yet)
        outColor = vec4(expand(now), cur.a);
        return;
    }
    // Where it was (the depth in the picture drawn)
    float depth = scaled ? texelFetch(depthMap, ivec2(vp.xy + (px - vp.xy) * scale), 0).r : texture(depthMap, vUV).r;
    vec2 ndc = (px - vp.xy) / vp.zw * 2.0 - 1.0;
    vec4 prev = pc.reproject * vec4(ndc, depth, 1.0);
    vec2 prevPx = (prev.xy / prev.w * 0.5 + 0.5) * vp.zw + vp.xy;
    bool seen = prevPx.x >= vp.x && prevPx.y >= vp.y && prevPx.x < vp.x + vp.z && prevPx.y < vp.y + vp.w;
    if (scaled && seen) {
        // Upscaling: this frame's drawn pixel whose (jittered) sample falls nearest this pixel, weighed by how near
        // (a Gaussian over the drawn pixel's size); the history keeps the rest. Over the jitter's eight positions
        // every full-size pixel gets its own samples: detail beyond the picture drawn.
        vec2 q = vp.xy + (px - vp.xy) * scale;   // (this pixel's middle, in drawn pixels)
        vec2 t = floor(q + pc.extra.xy);
        vec2 d = q - (t + 0.5 - pc.extra.xy);
        float w = exp(-2.29 * dot(d, d));
        vec3 sampled = compress(texelFetch(current, ivec2(t), 0).rgb);
        float alpha = min(1.0, (1.0 - pc.params.x) / (scale * scale)) * w;
        vec3 before = clamp(compress(texture(history, prevPx * pc.params.yz).rgb), lo, hi);
        outColor = vec4(expand(mix(before, sampled, alpha)), cur.a);
        return;
    }
    vec3 before = clamp(compress(texture(history, prevPx * pc.params.yz).rgb), lo, hi);
    outColor = vec4(expand(mix(now, before, seen ? pc.params.x : 0.0)), cur.a);   // (alpha: the outline's marker)
#endif
}
