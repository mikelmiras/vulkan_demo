#version 450
// The last pass: HDR scene + bloom, exposure, tonemapping (Khronos PBR Neutral: colours below ~0.8 stay as they
// are, brighter ones roll off), gamma encoding for the UNORM swapchain, and a little dithering against banding.
// The selection outline (alpha > 1 in the HDR image) is shown as is.

layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D ao;   // ambient occlusion, half size (when a.z = 1)
layout(set = 0, binding = 3) uniform sampler2D uiLayer;   // the UI, drawn apart (when b.w = 1): premultiplied, sRGB
layout(push_constant) uniform Post {
    vec4 a;   // x = exposure, y = bloom strength, z = 1: apply ambient occlusion
    vec4 b;   // rgb = outline colour (sRGB), w = 1: blend the UI layer in
    vec4 c;   // x = colour-blindness filter: 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia;
              // y = output: 0 SDR (gamma-encoded), 1 HDR10 (PQ, Rec.2020), 2 scRGB (linear, 1 = 80 nits);
              // z = the display's peak, w = paper white (nits; HDR)
    vec4 d;   // with the UI layer (drawn over the whole picture): the scene's rectangle (x, y, w, h pixels)
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

// Daltonization (Fidaner, Lin, Ozguven): the colour as that eye sees it (simulated in LMS cone space), and what's
// lost moved into the channels it still tells apart
vec3 daltonize(vec3 rgb, int mode) {
    const mat3 toLms = mat3(17.8824, 3.45565, 0.0299566, 43.5161, 27.1554, 0.184309, 4.11935, 3.86714, 1.46709);
    const mat3 toRgb = mat3(0.0809444479, -0.0102485335, -0.000365296938, -0.130504409, 0.0540193266, -0.00412161469,
                            0.116721066, -0.113614708, 0.693511405);
    vec3 lms = toLms * rgb;
    if (mode == 1) lms.x = 2.02344 * lms.y - 2.52581 * lms.z;         // no L cones
    else if (mode == 2) lms.y = 0.494207 * lms.x + 1.24827 * lms.z;   // no M cones
    else lms.z = -0.395913 * lms.x + 0.801109 * lms.y;                // no S cones
    vec3 error = rgb - toRgb * lms;
    vec3 shift = vec3(0.0, 0.7 * error.r + error.g, 0.7 * error.r + error.b);
    return rgb + shift;   // (SDR clamps to 0..1; HDR keeps what's over paper white)
}

// HDR10's encoding (render/display.cpp has the reference): nits -> PQ
vec3 pq(vec3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0,
                c3 = 2392.0 / 4096.0 * 32.0;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 h = texelFetch(hdr, px, 0);
    float outline = clamp(h.a - 1.0, 0.0, 1.0);   // (partly, on anti-aliased edges)
    vec3 scene = outline < 0.999 ? (h.rgb - outline * pc.b.rgb) / (1.0 - outline) : vec3(0.0);
    vec2 uv = (vec2(px) + 0.5) / vec2(textureSize(hdr, 0));
    bool outside = pc.b.w > 0.5 && (gl_FragCoord.x < pc.d.x || gl_FragCoord.y < pc.d.y || gl_FragCoord.x > pc.d.x + pc.d.z ||
                                    gl_FragCoord.y > pc.d.y + pc.d.w);   // (around the scene: its clear colour, as it is)
    if (pc.a.z > 0.5) {   // a little blur over the half-size occlusion: four bilinear taps
        vec2 t = 1.0 / vec2(textureSize(ao, 0));
        float occ = texture(ao, uv + vec2(-0.5, -0.5) * t).r + texture(ao, uv + vec2(0.5, -0.5) * t).r +
                    texture(ao, uv + vec2(-0.5, 0.5) * t).r + texture(ao, uv + vec2(0.5, 0.5) * t).r;
        scene *= occ * 0.25;
    }
    vec3 c = (max(scene, vec3(0.0)) + texture(bloom, uv).rgb * pc.a.y) * pc.a.x;
    int output_ = int(pc.c.y + 0.5);
    if (outside) {
        vec3 clear = h.rgb;   // (linear sky)
        vec4 ui = texelFetch(uiLayer, px, 0);
        if (output_ != 0) {
            vec3 uiLinear = ui.a > 0.0 ? pow(ui.rgb / ui.a, vec3(2.2)) * ui.a : vec3(0.0);
            vec3 nits = (clear * (1.0 - ui.a) + uiLinear) * pc.c.w;
            const mat3 m = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
            outColor = output_ == 1 ? vec4(pq(m * nits), 1.0) : vec4(nits / 80.0, 1.0);
        } else {
            outColor = vec4(pow(clear, vec3(1.0 / 2.2)) * (1.0 - ui.a) + ui.rgb, 1.0);
        }
        return;
    }
    if (output_ != 0) {   // HDR: highlights kept up to the display's peak, the UI at paper white, in linear light
        float peak = max(pc.c.z / pc.c.w, 1.0);   // (in paper whites)
        vec3 t = pbrNeutral(c / peak) * peak;
        if (pc.c.x > 0.5) t = max(daltonize(t, int(pc.c.x + 0.5)), vec3(0.0));   // (in linear light, paper whites)
        t = mix(t, pow(pc.b.rgb, vec3(2.2)), outline);
        vec4 ui = pc.b.w > 0.5 ? texelFetch(uiLayer, px, 0) : vec4(0.0);
        vec3 uiLinear = ui.a > 0.0 ? pow(ui.rgb / ui.a, vec3(2.2)) * ui.a : vec3(0.0);
        vec3 nits = (t * (1.0 - ui.a) + uiLinear) * pc.c.w;
        if (output_ == 1) {
            const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
            outColor = vec4(pq(to2020 * nits), 1.0);
        } else {
            outColor = vec4(nits / 80.0, 1.0);
        }
        return;
    }
    c = pow(clamp(pbrNeutral(c), 0.0, 1.0), vec3(1.0 / 2.2));
    if (pc.c.x > 0.5) c = clamp(daltonize(c, int(pc.c.x + 0.5)), 0.0, 1.0);
    c = mix(c, pc.b.rgb, outline);
    float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    c += noise / 255.0;
    if (pc.b.w > 0.5) {   // (the UI layer: over the picture, as ImGui would have blended it)
        vec4 ui = texelFetch(uiLayer, px, 0);
        c = c * (1.0 - ui.a) + ui.rgb;
    }
    outColor = vec4(c, 1.0);
}
