#version 450
// Bloom, going down the mip chain: a 13-tap filtered half-size copy (Jimenez, "Next Generation Post Processing in
// Call of Duty: Advanced Warfare"). The first pass also keeps only what's above the threshold, and weighs each
// block by its brightness (Karis average) so single bright pixels don't flicker.

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Post {
    vec4 a;   // xy = source texel size, z = 1 for the first pass, w = threshold
    vec4 b;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 fetch(vec2 offset) {
    vec4 c = texture(source, vUV + offset * pc.a.xy);
    // The selection outline is marked with alpha > 1: it doesn't glow
    return c.rgb * (1.0 - clamp(c.a - 1.0, 0.0, 1.0));
}

vec3 prefilter(vec3 c) {
    // One NaN or infinite pixel would spread over the whole chain (a black rectangle on screen): drop it
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    c = min(c, vec3(1e4));
    float l = luma(c);
    float knee = pc.a.w * 0.5;   // a soft start instead of a hard cut
    float soft = clamp(l - pc.a.w + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float w = max(soft, l - pc.a.w) / max(l, 1e-5);
    return c * w;
}

void main() {
    vec3 a = fetch(vec2(-2, -2)), b = fetch(vec2(0, -2)), c = fetch(vec2(2, -2));
    vec3 d = fetch(vec2(-2, 0)), e = fetch(vec2(0, 0)), f = fetch(vec2(2, 0));
    vec3 g = fetch(vec2(-2, 2)), h = fetch(vec2(0, 2)), i = fetch(vec2(2, 2));
    vec3 j = fetch(vec2(-1, -1)), k = fetch(vec2(1, -1)), l = fetch(vec2(-1, 1)), m = fetch(vec2(1, 1));
    vec3 result;
    if (pc.a.z > 0.5) {
        // Five overlapping 2x2 blocks, each weighted by 1 / (1 + luma)
        vec3 b0 = (j + k + l + m) * 0.25, b1 = (a + b + d + e) * 0.25, b2 = (b + c + e + f) * 0.25;
        vec3 b3 = (d + e + g + h) * 0.25, b4 = (e + f + h + i) * 0.25;
        float w0 = 0.5 / (1.0 + luma(b0)), w1 = 0.125 / (1.0 + luma(b1)), w2 = 0.125 / (1.0 + luma(b2));
        float w3 = 0.125 / (1.0 + luma(b3)), w4 = 0.125 / (1.0 + luma(b4));
        result = (b0 * w0 + b1 * w1 + b2 * w2 + b3 * w3 + b4 * w4) / (w0 + w1 + w2 + w3 + w4);
        result = prefilter(result);
    } else {
        result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    outColor = vec4(result, 1.0);
}
