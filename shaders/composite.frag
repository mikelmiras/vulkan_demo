#version 450
// The last pass: HDR scene + bloom, exposure, tonemapping (Khronos PBR Neutral: colours below ~0.8 stay as they
// are, brighter ones roll off), gamma encoding for the UNORM swapchain, and a little dithering against banding.
// The selection outline (alpha > 1 in the HDR image) is shown as is.

layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D ao;   // ambient occlusion, half size (when a.z = 1)
layout(push_constant) uniform Post {
    vec4 a;   // x = exposure, y = bloom strength, z = 1: apply ambient occlusion
    vec4 b;   // rgb = outline colour (sRGB)
} pc;

layout(location = 0) out vec4 outColor;

vec3 pbrNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 h = texelFetch(hdr, px, 0);
    float outline = clamp(h.a - 1.0, 0.0, 1.0);   // (partly, on anti-aliased edges)
    vec3 scene = outline < 0.999 ? (h.rgb - outline * pc.b.rgb) / (1.0 - outline) : vec3(0.0);
    vec2 uv = (vec2(px) + 0.5) / vec2(textureSize(hdr, 0));
    if (pc.a.z > 0.5) {   // a little blur over the half-size occlusion: four bilinear taps
        vec2 t = 1.0 / vec2(textureSize(ao, 0));
        float occ = texture(ao, uv + vec2(-0.5, -0.5) * t).r + texture(ao, uv + vec2(0.5, -0.5) * t).r +
                    texture(ao, uv + vec2(-0.5, 0.5) * t).r + texture(ao, uv + vec2(0.5, 0.5) * t).r;
        scene *= occ * 0.25;
    }
    vec3 c = (max(scene, vec3(0.0)) + texture(bloom, uv).rgb * pc.a.y) * pc.a.x;
    c = pow(clamp(pbrNeutral(c), 0.0, 1.0), vec3(1.0 / 2.2));
    c = mix(c, pc.b.rgb, outline);
    float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    outColor = vec4(c + noise / 255.0, 1.0);
}
