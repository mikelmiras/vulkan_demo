#version 450
// Auto-exposure, first pass: the view's brightness, as log2 of luminance, into a small image (each texel averaging
// four points of its part of the view); its mip chain (blits) then averages it down to one texel: the geometric mean.

layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(push_constant) uniform Push {
    vec4 a;   // the view's rectangle in the image (pixels)
    vec4 b;   // xy = 1 / image size
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 o = vec2(float(i & 1), float(i >> 1)) * 0.5 - 0.25;   // (a 2 x 2 grid in this texel's part)
        vec2 p = pc.a.xy + (vUV + o * fwidth(vUV)) * pc.a.zw;
        vec3 c = texture(hdr, p * pc.b.xy).rgb;
        float lum = dot(min(c, vec3(64.0)), vec3(0.2126, 0.7152, 0.0722));
        sum += clamp(log2(max(lum, 1e-5)), -14.0, 8.0);   // (a few extremes can't pull it far)
    }
    outColor = vec4(sum * 0.25, 0.0, 0.0, 1.0);
}
