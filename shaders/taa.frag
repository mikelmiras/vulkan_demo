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
layout(set = 0, binding = 3) uniform sampler2D velocityMap;   // each drawn pixel's motion since last frame (uv)
#endif
layout(push_constant) uniform Push {
    mat4 reproject;   // this frame's clip space -> last frame's
    vec4 viewport;    // the scene's rectangle in the image (pixels: full size)
    vec4 params;      // x = how much history (0: none yet), y, z = 1 / image size, w = render scale
    vec4 extra;       // xy = this frame's jitter (drawn pixels), z = sharpening (0: none), w = 1: motion vectors
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Blended in a compressed range: one very bright pixel doesn't outweigh the rest (and flicker)
vec3 compress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 expand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
// The history pulled towards the middle of the colours around now (clipped to their box along the line to its
// centre, rather than clamped per channel: no colour shift at the box's edge)
vec3 clipToBox(vec3 lo, vec3 hi, vec3 c) {
    vec3 centre = 0.5 * (lo + hi), extent = 0.5 * (hi - lo) + 1e-5;
    vec3 v = c - centre, a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : c;
}

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
    // The colours around this pixel now (in the picture drawn): their range, and their mean and spread (variance
    // clipping: the history kept within 1.25 standard deviations of the mean, inside the range, so flickering
    // and ghosting colours are pulled in harder than by the range alone), and the cross's mean for sharpening
    vec3 lo = vec3(1e9), hi = vec3(-1e9), plus = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 c = compress(texture(current, at + vec2(x, y) * pc.params.yz).rgb);
            lo = min(lo, c);
            hi = max(hi, c);
            m1 += c;
            m2 += c * c;
            if (abs(x) + abs(y) == 1) plus += c;
        }
    vec3 mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 boxLo = max(lo, mean - 1.25 * sigma), boxHi = min(hi, mean + 1.25 * sigma);
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
    if (pc.extra.w > 0.5) {   // moving things: where they were, by their own motion (the sky's: by the depth)
        // (the nearest thing's motion round this pixel: an edge moves with what's in front, not what's behind)
        ivec2 centre = ivec2(scaled ? vp.xy + (px - vp.xy) * scale : px), size = textureSize(depthMap, 0);
        ivec2 nearest = centre;
        float nearestDepth = 2.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                ivec2 q = clamp(centre + ivec2(x, y), ivec2(0), size - 1);
                float d = texelFetch(depthMap, q, 0).r;
                if (d < nearestDepth) nearestDepth = d, nearest = q;
            }
        vec2 v = texelFetch(velocityMap, nearest, 0).rg;
        if (v.x < 10000.0) prevPx = px - v * vp.zw;
    }
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
        vec3 before = clipToBox(boxLo, boxHi, compress(texture(history, prevPx * pc.params.yz).rgb));
        outColor = vec4(expand(mix(before, sampled, alpha)), cur.a);
        return;
    }
    vec3 before = clipToBox(boxLo, boxHi, compress(texture(history, prevPx * pc.params.yz).rgb));
    outColor = vec4(expand(mix(now, before, seen ? pc.params.x : 0.0)), cur.a);   // (alpha: the outline's marker)
#endif
}
