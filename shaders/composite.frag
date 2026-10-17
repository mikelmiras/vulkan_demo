#version 450
// The last pass: HDR scene + bloom, exposure, tonemapping (Khronos PBR Neutral: colours below ~0.8 stay as they
// are, brighter ones roll off), gamma encoding for the UNORM swapchain, and a little dithering against banding.
// The selection outline (alpha > 1 in the HDR image) is shown as is.

layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D ao;   // ambient occlusion, half size (when a.z = 1)
layout(set = 0, binding = 3) uniform sampler2D uiLayer;   // the UI, drawn apart (when b.w = 1): premultiplied, sRGB
layout(set = 0, binding = 4) uniform sampler2D exposureMap;   // auto-exposure: log2 of its factor (one texel)
layout(set = 0, binding = 5) uniform sampler2D luminance;     // the view's log2 luminance, mipped (local tone mapping)
layout(set = 0, binding = 6) uniform sampler2D lensDirt;      // smudges and specks the bloom catches (renderer_post.cpp)
layout(set = 0, binding = 7) uniform sampler2D shafts;        // the light shafts, half size (when a.w = 1: shafts.glsl; a = what
                                                              // the air leaves of the picture)
layout(push_constant) uniform Post {
    vec4 a;   // x = exposure, y = bloom strength, z = 1: apply ambient occlusion, w = 1: add the light shafts
    vec4 b;   // rgb = outline colour (sRGB), w = 1: blend the UI layer in
    vec4 c;   // x = colour-blindness filter: 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia;
              // y = output: 0 SDR (gamma-encoded), 1 HDR10 (PQ, Rec.2020), 2 scRGB (linear, 1 = 80 nits);
              // z = the display's peak, w = paper white (nits; HDR)
    vec4 d;   // with the UI layer (drawn over the whole picture): the scene's rectangle (x, y, w, h pixels)
    vec4 e;   // x = tone mapper (0 PBR Neutral, 1 filmic), y = contrast, z = saturation, w = vignette
    vec4 f;   // x = 1: auto-exposure, y = local tone mapping (0..1), z = the luminance's last mip, w = lens dirt
    vec4 g;   // x = lens flare (0..1), y = the glow's scale (more bloom levels: wider, not brighter), z = sharpening
              // (0..1: temporal AA's or the upscaler's softness taken back)
} pc;

// A lens's flare (after Chapman's pseudo lens flare): the brightest spots (what's over 1 in the bloom's blurred
// levels, compressed) mirrored through the view's middle as a row of ghosts (each fading towards the edge, its colours a little
// apart, as glass splits them) and a ring round the middle (the halo) where a bright spot's light comes from the side
vec3 brightest(vec2 at, float level) {
    if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0) return vec3(0.0);   // (off the picture: nothing)
    vec3 over = max(textureLod(bloom, at, level).rgb - vec3(1.0), vec3(0.0));
    return over / (1.0 + dot(over, vec3(0.3333)));   // (compressed: one blinding highlight can't flood the view)
}
vec3 lensFlare(vec2 uv) {
    vec2 flipped = vec2(1.0) - uv;
    vec2 ghostStep = (vec2(0.5) - flipped) * 0.38;
    vec3 sum = vec3(0.0);
    for (int i = 1; i < 5; ++i) {
        vec2 at = flipped + ghostStep * float(i);
        float w = pow(1.0 - clamp(length(vec2(0.5) - at) / 0.7071, 0.0, 1.0), 6.0) / float(i);   // (the further, the fainter)
        vec2 apart = normalize(vec2(0.5) - at + 1e-5) * 0.004 * float(i);
        sum += vec3(brightest(at + apart, 2.0).r, brightest(at, 2.0).g, brightest(at - apart, 2.0).b) * w;
    }
    vec2 aspect = vec2(textureSize(bloom, 0)) / float(textureSize(bloom, 0).y);   // (the halo round, not stretched)
    vec2 at = flipped + normalize((ghostStep + 1e-5) * aspect) * 0.3 / aspect;
    float w = pow(1.0 - clamp(length(vec2(0.5) - at) / 0.7071, 0.0, 1.0), 4.0) * 0.15;
    sum += brightest(at, 3.0) * w * vec3(0.8, 0.9, 1.0);
    return sum;
}

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

// Filmic: AgX (Troy Sobotka's; the polynomial fit of its contrast curve by Benjamin Wrensch): into its working
// space, a log encoding over 16.5 stops, the curve, and back to linear. Highlights desaturate towards white as film's do.
vec3 agx(vec3 c) {
    const mat3 toAgx = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772,
                            0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 fromAgx = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417,
                              -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    vec3 x = clamp((log2(max(toAgx * c, vec3(1e-10))) - minEv) / (maxEv - minEv), 0.0, 1.0);
    vec3 x2 = x * x, x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    return pow(max(fromAgx * x, vec3(0.0)), vec3(2.2));
}

// Grading in linear light: contrast around middle grey (in stops), saturation around the colour's luminance, and
// the corners darkened (the vignette, by the distance from the picture's middle)
vec3 grade(vec3 c, vec2 uv) {
    float contrast = pc.e.y, saturation = pc.e.z, vignette = pc.e.w;
    if (contrast != 1.0) c = 0.18 * pow(max(c, vec3(0.0)) / 0.18, vec3(contrast));
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = max(mix(vec3(luma), c, saturation), vec3(0.0));
    if (vignette > 0.0) {
        vec2 v = (uv - 0.5) * vec2(pc.d.z / max(pc.d.w, 1.0), 1.0);
        c *= 1.0 - vignette * smoothstep(0.35, 1.0, length(v) * 1.15);
    }
    return c;
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

// Contrast-adaptive sharpening (after AMD's FidelityFX CAS) of the picture temporal AA made: each pixel pushed away
// from its four neighbours by as much as their contrast leaves room for (less where it's already sharp or near the
// range's ends, so no halos). Here rather than in a pass of its own (the history stays unsharpened, as before).
vec3 casCompress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 casExpand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
vec3 sharpened(ivec2 p, vec3 centre, float amount) {
    ivec2 hi = textureSize(hdr, 0) - 1;
    vec3 e = casCompress(centre);
    vec3 b = casCompress(texelFetch(hdr, clamp(p + ivec2(0, -1), ivec2(0), hi), 0).rgb);
    vec3 d = casCompress(texelFetch(hdr, clamp(p + ivec2(-1, 0), ivec2(0), hi), 0).rgb);
    vec3 f = casCompress(texelFetch(hdr, clamp(p + ivec2(1, 0), ivec2(0), hi), 0).rgb);
    vec3 h = casCompress(texelFetch(hdr, clamp(p + ivec2(0, 1), ivec2(0), hi), 0).rgb);
    vec3 mn = min(e, min(min(b, d), min(f, h))), mx = max(e, max(max(b, d), max(f, h)));
    vec3 room = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 w = room * (-1.0 / mix(8.0, 5.0, clamp(amount, 0.0, 1.0)));
    vec3 sharp = clamp((b + d + f + h) * w + e, 0.0, 1.0) / (1.0 + 4.0 * w);
    return casExpand(clamp(sharp, 0.0, 0.999));
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 h = texelFetch(hdr, px, 0);
    bool inScene = pc.b.w < 0.5 || (gl_FragCoord.x >= pc.d.x && gl_FragCoord.y >= pc.d.y && gl_FragCoord.x <= pc.d.x + pc.d.z &&
                                    gl_FragCoord.y <= pc.d.y + pc.d.w);
    if (pc.g.z > 0.0 && h.a <= 1.0 && inScene) h.rgb = sharpened(px, h.rgb, pc.g.z);   // (not the outline's marker)
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
    // (the glow, and where the lens is dirty, more of it: smudges lit up by bright lights)
    vec3 glow = texture(bloom, uv).rgb * pc.a.y * pc.g.y;
    if (pc.g.x > 0.0) glow += lensFlare(uv) * (pc.g.x * 0.06);
    if (pc.f.w > 0.0) glow *= 1.0 + texture(lensDirt, uv).rgb * (pc.f.w * 6.0);
    vec3 c = max(scene, vec3(0.0)) + glow;
    if (pc.a.w > 0.5 && !outside) {   // (a little blur over the half-size shafts, as over the occlusion: their march's dither)
        vec2 t = 1.0 / vec2(textureSize(shafts, 0));
        c *= texture(shafts, uv).a;   // (what the air left of the picture behind: a, unblurred)
        c += 0.25 * (texture(shafts, uv + vec2(-0.75, -0.75) * t).rgb + texture(shafts, uv + vec2(0.75, -0.75) * t).rgb +
                     texture(shafts, uv + vec2(-0.75, 0.75) * t).rgb + texture(shafts, uv + vec2(0.75, 0.75) * t).rgb);
    }
    float exposure = pc.a.x;
    if (pc.f.x > 0.5) exposure *= exp2(texelFetch(exposureMap, ivec2(0), 0).r);   // (auto: times the compensation)
    if (pc.f.y > 0.0) {   // local tone mapping: this part of the view's brightness (coarse) pulled towards the mean
        // (the brightness around it from the coarse level, 3 x 3 texels each weighed by how near its brightness is to
        // this pixel's: an edge-aware average, so a dark wall beside a bright sky isn't lit up along the edge)
        vec2 at = clamp((gl_FragCoord.xy - pc.d.xy) / max(pc.d.zw, vec2(1.0)), 0.0, 1.0);
        float mine = log2(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-6));
        vec2 texel = 1.0 / vec2(textureSize(luminance, 3));
        float local = 0.0, weights = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                float l = textureLod(luminance, at + vec2(x, y) * texel, 3.0).r;
                float w = exp(-(l - mine) * (l - mine) * 0.5) * (x == 0 && y == 0 ? 2.0 : 1.0);
                local += l * w;
                weights += w;
            }
        local /= max(weights, 1e-4);
        float mean = textureLod(luminance, vec2(0.5), pc.f.z).r;
        c *= exp2(clamp(mean - local, -6.0, 6.0) * pc.f.y * 0.5);
    }
    c *= exposure;
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
        vec3 t = grade(pbrNeutral(c / peak) * peak, (gl_FragCoord.xy - pc.d.xy) / max(pc.d.zw, vec2(1.0)));
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
    c = pc.e.x > 0.5 ? agx(c) : pbrNeutral(c);
    c = grade(c, (gl_FragCoord.xy - pc.d.xy) / max(pc.d.zw, vec2(1.0)));
    c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
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
