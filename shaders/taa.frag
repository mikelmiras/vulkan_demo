#version 450
// Temporal anti-aliasing: this frame (drawn with a sub-pixel jitter) blended with the frames before it. The history
// is looked up where this pixel's point was last frame (reprojected through the depth) and clamped to the colours
// around this pixel now, so it can't drag old colours along (ghosting) when things move.

layout(set = 0, binding = 0) uniform sampler2D current;
layout(set = 0, binding = 1) uniform sampler2D history;
layout(set = 0, binding = 2) uniform sampler2D depthMap;
layout(push_constant) uniform Push {
    mat4 reproject;   // this frame's clip space -> last frame's
    vec4 viewport;    // the scene's rectangle in the image (pixels)
    vec4 params;      // x = how much history (0: none yet), y, z = 1 / image size
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Blended in a compressed range: one very bright pixel doesn't outweigh the rest (and flicker)
vec3 compress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 expand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

void main() {
    vec4 cur = texture(current, vUV);
    vec2 px = gl_FragCoord.xy;
    vec4 vp = pc.viewport;
    bool inView = px.x >= vp.x && px.y >= vp.y && px.x < vp.x + vp.z && px.y < vp.y + vp.w;
    if (pc.params.x <= 0.0 || !inView) {
        outColor = cur;
        return;
    }
    // The colours around this pixel now
    vec3 lo = vec3(1e9), hi = vec3(-1e9);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 c = compress(texture(current, vUV + vec2(x, y) * pc.params.yz).rgb);
            lo = min(lo, c);
            hi = max(hi, c);
        }
    // Where it was
    float depth = texture(depthMap, vUV).r;
    vec2 ndc = (px - vp.xy) / vp.zw * 2.0 - 1.0;
    vec4 prev = pc.reproject * vec4(ndc, depth, 1.0);
    vec2 prevPx = (prev.xy / prev.w * 0.5 + 0.5) * vp.zw + vp.xy;
    bool seen = prevPx.x >= vp.x && prevPx.y >= vp.y && prevPx.x < vp.x + vp.z && prevPx.y < vp.y + vp.w;
    vec3 now = compress(cur.rgb);
    vec3 before = clamp(compress(texture(history, prevPx * pc.params.yz).rgb), lo, hi);
    outColor = vec4(expand(mix(now, before, seen ? pc.params.x : 0.0)), cur.a);   // (alpha: the outline's marker)
}
