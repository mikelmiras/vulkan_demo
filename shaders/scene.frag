#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "tint.glsl"
#include "fog.glsl"
#include "screen_trace.glsl"
#ifdef EARLY_DEPTH
// (scene_early.frag.spv: after the depth prepass, only the pixel in front is shaded; the depth test before the
// shader, which its texture-streaming writes would otherwise put after it)
layout(early_fragment_tests) in;
#endif
// Lit objects: metallic-roughness PBR (GGX), sun with shadows, point lights, hemisphere ambient, fog.
// Writes linear HDR colour (alpha 1); the post pass adds bloom, tonemaps and gamma-encodes.

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowCascades;   // the sun's, 4 layers (comparing)
layout(set = 0, binding = 5) uniform sampler2DShadow shadowAtlas;          // spot and point lights' (tiles; comparing)
layout(set = 0, binding = 42) uniform sampler2D atlasDepths;              // the same, as depths (lamps' blocker search)
layout(set = 0, binding = 20) uniform sampler2DArray cascadeDepths;        // the sun's again, as depths (PCSS)
layout(set = 0, binding = 21) uniform sampler3D fogVolume;                 // volumetric fog, integrated (fog.glsl)
layout(set = 0, binding = 22) uniform sampler2D fogSurface;                // ... each cell column's surface distance
layout(set = 0, binding = 6) uniform sampler2DArray cookies;          // spot lights' cookies
// Point and spot lights, and the clusters of the view they reach (render/light_clusters.h)
struct Light {
    vec4 posRange;   // xyz, w = range
    vec4 color;      // rgb = colour * intensity (linear), w = 1: spot
    vec4 dir;        // spots: the direction it shines, w = cos outer
    vec4 params;     // x = cos inner, y = first shadow view (-1: none), z = cookie layer (-1: none), w = tan outer
    vec4 shape;      // x = its size (the bulb's radius, m: lamps' soft shadows)
};
layout(std430, set = 0, binding = 14) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 15) readonly buffer Clusters { uint clusters[]; };   // (offset, count) each, then indices
layout(set = 0, binding = 9) uniform sampler2D probe0;         // reflection probes' radiance (like skyRadiance)
layout(set = 0, binding = 10) uniform sampler2D probe1;
layout(set = 0, binding = 11) uniform sampler2D probe2;
layout(set = 0, binding = 12) uniform sampler2D probe3;
// Dynamic GI probes (renderer_gi.cpp): per slot, position (w = 1: drawn), then nine spherical harmonics
layout(std430, set = 0, binding = 17) readonly buffer GiProbes { vec4 giProbes[]; };
// Texture streaming: the finest mip level each texture was wanted at this frame (texture_streaming.h)
layout(std430, set = 0, binding = 19) buffer TextureFeedback { uint wantedLevel[]; };
layout(set = 0, binding = 46) uniform sampler2D cloudShadowMap; // the volumetric clouds' shadow (cloud_shadow.frag)
layout(set = 1, binding = 0) uniform sampler2D albedoMap;      // sRGB view (white if none)
layout(set = 1, binding = 1) uniform sampler2D normalMap;      // tangent space (flat if none)
layout(set = 1, binding = 2) uniform sampler2D metalRoughMap;  // g = roughness, b = metallic (white if none)
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;    // sRGB view (white if none)
layout(set = 1, binding = 4) uniform sampler2D heightMap;      // parallax occlusion: r = height (white if none)
layout(set = 1, binding = 5) uniform sampler2D detailMap;      // a fine normal map, tiled (flat if none)
layout(set = 1, binding = 6) uniform sampler2D layerNormals[4]; // a terrain's layers' normal maps (flat if none)
// Decals (renderer_decals.cpp): the nearest, each a box projected down onto the surfaces in it, and their textures
struct GpuDecal {
    mat4 toBox;     // world -> the box (-0.5..0.5; it projects down its -y: u along x, v along z)
    vec4 color;     // rgb linear, a = opacity
    vec4 params;    // x = texture slot (-1: none), y = roughness (-1: as it is), z = wetness, w = angle fade
    vec4 down;      // xyz: its direction in the world
    vec4 sphere;    // round its box: centre, radius squared
    vec4 normal;    // x = normal map slot (-1: none), y = its strength, z = its glow (0: none)
};
layout(std430, set = 0, binding = 24) readonly buffer Decals { GpuDecal decals[]; };
layout(set = 0, binding = 25) uniform sampler2D decalTextures[16];
layout(std430, set = 0, binding = 27) readonly buffer DecalClusters { uint decalClusters[]; };   // (as the lights')
layout(set = 0, binding = 28) uniform sampler2D decalNormals[8];
// Water's refraction (renderer.cpp): the opaque picture (rgb) and its distances along the view (a), copied before
// the see-through things are drawn
layout(set = 0, binding = 26) uniform sampler2D refraction;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in vec4 vColor;
layout(location = 4) flat in vec4 vParams;
layout(location = 5) flat in vec4 vMaterial;
layout(location = 6) flat in vec4 vEmissive;
layout(location = 7) in vec4 vSplat;
layout(location = 8) in float vHole;
layout(location = 9) flat in vec4 vStream;
layout(location = 10) flat in vec4 vLayers;   // clear coat, its roughness, sheen, anisotropy
layout(location = 11) flat in vec4 vSurface;  // parallax depth, detail tiling, detail strength, w = 1: sheltered
layout(location = 14) flat in vec4 vScatter;  // translucency, subsurface, its colour (packed: unpackColor)

vec2 gUV;   // the texture coordinates the material is read at (vUV, moved by parallax occlusion)

// Reflections and GI traced at half size beforehand (renderer_trace.cpp; frame.trace.x = 1): this pixel's, brought
// up from the four half-size pixels round it, each weighed by how near its depth is to this one's (a bilateral
// upsample: no bleeding across edges)
layout(set = 0, binding = 31) uniform sampler2D tracedReflections;   // rgb, a = how much to trust it
layout(set = 0, binding = 32) uniform sampler2D tracedLight;         // rgb = what the GI rays met, a = how much
layout(set = 0, binding = 37) uniform sampler2D occlusionMap;       // ambient occlusion before lighting (half size): r =
                                                                     // occlusion, g = distance (0: none), ba = bent normal
layout(set = 0, binding = 35) uniform sampler2D tracedDepth;         // r = the traced pixel's distance along the view,
                                                                     // g = its contact shadow (1: lit), b = the lamp's
// Subsurface pixels' specular light, kept apart (frame.occlusion.y = 1: the main view, diffusion on) so the diffusion
// (sss.glsl) spreads their diffuse light alone: rgb = the specular as half floats, a = the material's profile (how
// far each channel spreads, 5:6:5)
layout(set = 0, binding = 41, rgba16ui) uniform writeonly uimage2D skinSpecular;
bool halfTraced = false;
vec4 tracedReflection = vec4(0.0), tracedIrradiance = vec4(0.0);
float tracedContact = 1.0;   // (the sun's)
vec4 tracedLampContacts = vec4(1.0);   // (frame.contactLights')
vec4 unpackContacts(vec2 ba) {   // (contact_lamps.cpp's unpack3, and the fourth)
    float p = round(ba.x);   // (not floor(p + 0.5): near 2^24 the half is lost)
    return vec4(mod(p, 256.0), mod(floor(p / 256.0), 256.0), floor(p / 65536.0), 0.0) / 255.0 + vec4(0.0, 0.0, 0.0, ba.y);
}
// (`rough`: on a rough surface (render/gi_denoise.cpp's ssrBlurShare: from 0.15, fully from 0.4) the reflections'
// four from the next ring out too, depth-weighed alike: their grain evened out where the highlight is broad anyway;
// smooth ones (a mirror, wet asphalt) keep their own sharp reflection and pay nothing)
void readTraced(float rough) {
    vec2 size = vec2(textureSize(tracedDepth, 0));
    vec2 hp = gl_FragCoord.xy * 0.5 - 0.5;
    ivec2 b = ivec2(floor(hp));
    vec2 f = hp - vec2(b);
    float myZ = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
    vec4 r = vec4(0.0), g = vec4(0.0);
    float total = 0.0, nearest = 1e9, contact = 0.0, nearC = 1.0;
    vec4 lamp = vec4(0.0), nearL = vec4(1.0);
    vec4 nearR = vec4(0.0), nearG = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        ivec2 o = ivec2(i & 1, i >> 1);
        ivec2 c = clamp(b + o, ivec2(0), ivec2(size) - 1);
        vec4 zc4 = texelFetch(tracedDepth, c, 0);
        vec3 zc = zc4.rgb;
        vec4 lc = unpackContacts(zc4.ba);
        float z = zc.x;
        float off = abs(z - myZ) / max(myZ, 0.1);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) / (1.0 + off * off * 2500.0);
        vec4 tr = texelFetch(tracedReflections, c, 0), tg = texelFetch(tracedLight, c, 0);
        r += tr * w;
        g += tg * w;
        contact += zc.y * w;
        lamp += lc * w;
        total += w;
        if (off < nearest) { nearest = off; nearR = tr; nearG = tg; nearC = zc.y; nearL = lc; }
    }
    // (none near this depth, a thin thing: the nearest in depth)
    tracedReflection = total > 1e-3 ? r / total : nearR;
    float share = clamp((rough - 0.15) / 0.25, 0.0, 1.0);
    if (share > 0.0 && total > 1e-3) {   // (the ring's average blended in: half of it at full share)
        float wide = 0.0;
        vec4 rw = vec4(0.0);
        for (int i = 0; i < 4; ++i) {
            ivec2 c = clamp(b + ivec2(i & 1, i >> 1) * 3 - 1, ivec2(0), ivec2(size) - 1);   // (the ring's corners)
            float off = abs(texelFetch(tracedDepth, c, 0).r - myZ) / max(myZ, 0.1);
            float w = 1.0 / (1.0 + off * off * 2500.0);
            rw += texelFetch(tracedReflections, c, 0) * w;
            wide += w;
        }
        if (wide > 1e-3) tracedReflection = mix(tracedReflection, rw / wide, 0.5 * share);
    }
    tracedIrradiance = total > 1e-3 ? g / total : nearG;
    tracedContact = total > 1e-3 ? contact / total : nearC;
    tracedLampContacts = total > 1e-3 ? lamp / total : nearL;
}

layout(location = 0) out vec4 outColor;

// (false: the lean variant, for materials without layers: coat, sheen, anisotropy, translucency, subsurface are
// none, and their code folds away; the renderer draws each batch with the one it needs)
layout(constant_id = 0) const bool kLayers = true;
// (false: without the sun's coloured shadows, for frames with none: their lookups folded away too)
layout(constant_id = 1) const bool kTint = true;

const float PI = 3.14159265;

// Soft shadows: taps on a Poisson disk, turned by a per-pixel angle (interleaved gradient noise, moving each frame
// so temporal AA averages it out), each one a hardware-filtered 2 x 2 comparison: smooth edges from few taps
const vec2 kDisk[16] = vec2[](vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845),
    vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023),
    vec2(0.79197514, 0.19090188), vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));
mat2 diskRotation() {
    // (without temporal AA to average it away the pattern stays still: a fine, steady grain rather than one that
    // crawls, or the disk's copies showing through wide penumbrae)
    float moving = frame.shadowParams.y > 0.5 ? 5.588238 * frame.time.y : 0.0;
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy + moving, vec2(0.06711056, 0.00583715))));
    float a = n * 6.2831853;
    return mat2(cos(a), sin(a), -sin(a), cos(a));
}

// A 2 x 2 comparison, bilinearly weighted: four depth tests at once (textureGather), mixed by where the point falls
// between their texels. The same as a linear-filtering comparison sampler, on GPUs whose depth formats don't filter
// (Apple's, through MoltenVK)
float cascadeTap(vec2 uv, float layer, float z) {
    vec2 size = vec2(textureSize(shadowCascades, 0).xy);
    vec2 f = fract(uv * size - 0.5);
    vec4 g = textureGather(shadowCascades, vec3(uv, layer), z);   // (x: -,+ y: +,+ z: +,- w: -,-)
    return mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y);
}
float atlasTap(vec2 uv, float z) {
    vec2 size = vec2(textureSize(shadowAtlas, 0));
    vec2 f = fract(uv * size - 0.5);
    vec4 g = textureGather(shadowAtlas, uv, z);
    return mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y);
}

// The sun's cascade `c` at `p` (world): the blockers' average depth around it (PCSS), how wide the penumbra is from
// that and the sun's size, then the disk filtered at that width (at least a texel and a half: no stair steps)
float cascadeShadow(int c, vec3 p, float bias) {
    vec3 proj = (frame.shadowViewProj[c] * vec4(p, 1.0)).xyz;   // (orthographic: no divide)
    vec2 uv = proj.xy * 0.5 + 0.5;
    float z = proj.z - bias;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0 || proj.z < 0.0) return 1.0;
    vec4 scale = frame.cascadeScale[c];
    float texel = 1.0 / scale.z;
    mat2 rot = diskRotation();
    // A penumbra grows this many metres per metre behind what casts it: softness 1 is a sun about 2.3 degrees
    // across (a hazy one; the real sun's 0.53 makes edges too sharp to see soften)
    float size = 0.04 * frame.shadowParams.x;
    float radius = 1.5 * texel;
    // Quality: high 12 blocker taps and 16 filter taps, medium 6 and 8, low no blocker search (a fixed edge) and 4
    int quality = int(frame.shadowParams.z + 0.5);
    int taps = quality >= 2 ? 16 : quality == 1 ? 8 : 4, searches = quality >= 2 ? 12 : 6;
    if (size > 0.0 && quality > 0) {
        float search = clamp(size * 20.0 / scale.x, 2.0 * texel, 64.0 * texel);   // (casters up to ~20 m away)
        float sum = 0.0, n = 0.0;
        for (int i = 0; i < searches; ++i) {
            float d = textureLod(cascadeDepths, vec3(uv + rot * kDisk[i] * search, float(c)), 0.0).r;
            if (d < z) { sum += d; n += 1.0; }
        }
        if (n == 0.0) return 1.0;   // (nothing between it and the sun)
        float behind = (z - sum / n) * scale.y;   // metres from the blockers
        radius = clamp(behind * size / scale.x, 1.5 * texel, (frame.shadowParams.y > 0.5 ? 64.0 : 16.0) * texel);
    }
    float lit = 0.0;
    for (int i = 0; i < taps; ++i) lit += cascadeTap(uv + rot * kDisk[i * (16 / taps)] * radius, float(c), z);
    return lit / float(taps);
}

// The same in a tile of the atlas (`view`'s rectangle), a fixed width, its taps kept inside the tile
float atlasShadow(int view, vec3 p, float bias) {
    vec4 clip = frame.shadowViewProj[view] * vec4(p, 1.0);
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0 || proj.z < 0.0) return 1.0;
    vec4 rect = frame.shadowRect[view];
    vec2 texel = 1.0 / vec2(textureSize(shadowAtlas, 0));
    vec2 lo = rect.xy + texel * 0.5, hi = rect.xy + rect.zw - texel * 0.5;
    vec2 at = rect.xy + uv * rect.zw;
    mat2 rot = diskRotation();
    int taps = frame.shadowParams.z > 1.5 ? 12 : frame.shadowParams.z > 0.5 ? 6 : 4;   // (by quality)
    float lit = 0.0;
    for (int i = 0; i < taps; ++i)
        lit += atlasTap(clamp(at + rot * kDisk[i * (12 / taps)] * texel * 1.75, lo, hi), proj.z - bias);
    return lit / float(taps);
}

// A blocker's fraction of the way from the light to the point, from its depth; the penumbra there (render/
// lamp_shadow.cpp's, the same sums)
float blockerFraction(float d, float clipZ, float clipW, float lightZ) {
    float denom = d * clipW - clipZ + lightZ;
    return abs(denom) > 1e-12 ? lightZ / denom : 1.0;
}
float penumbra(float size, float t) {
    t = clamp(t, 0.02, 1.0);
    return size * (1.0 - t) / t;
}

// A lamp's shadow in its tile (`view`) at `p`, softened by the lamp's size (PCSS, as the sun's): the blockers round
// the point and how far from the light they are (each depth turned back into a fraction of the way from the light to
// the point, exactly, through the tile's projection), the penumbra a light `size` m round leaves at the point, and
// the disk filtered that wide (from a texel and three quarters: as without a size, up to 24 texels). Low quality:
// the fixed width.
float lampShadow(int view, vec3 p, float bias, vec3 lightPos, float size) {
    vec4 clip = frame.shadowViewProj[view] * vec4(p, 1.0);
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0 || proj.z < 0.0) return 1.0;
    vec4 rect = frame.shadowRect[view];
    vec2 texel = 1.0 / vec2(textureSize(shadowAtlas, 0));
    vec2 lo = rect.xy + texel * 0.5, hi = rect.xy + rect.zw - texel * 0.5;
    vec2 at = rect.xy + uv * rect.zw;
    mat2 rot = diskRotation();
    int quality = int(frame.shadowParams.z + 0.5);
    int taps = quality >= 2 ? 12 : quality == 1 ? 6 : 4;
    float radius = 1.75;   // (texels)
    if (size > 0.0 && quality > 0) {
        // (texels a metre across spans here: a step sideways, through the projection)
        vec3 axis = normalize(p - lightPos);
        vec3 side = normalize(cross(axis, abs(axis.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec4 c2 = frame.shadowViewProj[view] * vec4(p + side * 0.1, 1.0);
        float perMetre = length((c2.xy / c2.w - proj.xy) * 0.5 * rect.zw / texel) * 10.0;
        float search = clamp(size * perMetre, 1.5, 24.0);
        float b = (frame.shadowViewProj[view] * vec4(lightPos, 1.0)).z;   // (the light's own depth term)
        // (the search in 2 x 2 blocks: a gather reads four depths for the price of one; four blocks spread over the
        // disk at high quality, three at medium: half the reads of single taps, twice the depths)
        int searches = quality >= 2 ? 4 : 3;
        float sum = 0.0, n = 0.0;
        for (int i = 0; i < searches; ++i) {
            vec4 d4 = textureGather(atlasDepths, clamp(at + rot * kDisk[i * (quality >= 2 ? 4 : 5)] * search * texel, lo + texel * 0.5, hi - texel * 0.5), 0);   // (all four inside the tile)
            for (int k = 0; k < 4; ++k) {
                float d = d4[k];
                if (d < proj.z - bias) {   // (a blocker: how far along from the light, as a fraction of the point's way)
                    sum += blockerFraction(d, clip.z, clip.w, b);
                    n += 1.0;
                }
            }
        }
        if (n == 0.0) return 1.0;   // (nothing between it and the light)
        radius = clamp(penumbra(size, sum / n) * perMetre, 1.75, 24.0);
    }
    float lit = 0.0;
    for (int i = 0; i < taps; ++i)
        lit += atlasTap(clamp(at + rot * kDisk[i * (12 / taps)] * texel * radius, lo, hi), proj.z - bias);
    return lit / float(taps);
}

// The sun: the cascade covering this distance from the camera (beyond the last one: lit), blended into the next
// over the last part of each, so where one hands over to the next doesn't show
float sunVisibility(vec3 N, vec3 L) {
    if (frame.sunDir.w < 0.5) return 1.0;
    float d = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
    int c = 0;
    while (c < 4 && d > frame.cascadeSplits[c]) ++c;
    if (c >= 4) return 1.0;
    // Nudged along the normal and towards the light (more for the bigger, coarser cascades): no shadow acne
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    vec3 p = vWorldPos + N * (0.03 + 0.03 * float(c)) * (1.5 - NdotL) + L * 0.02;
    if (frame.viewport.w > 0.5) return atlasShadow(c, p + N * 0.03, 0.0005);   // (a split-screen view's: coarser tiles)
    float vis = cascadeShadow(c, p, 0.0005);
    float start = c == 0 ? 0.0 : frame.cascadeSplits[c - 1], end = frame.cascadeSplits[c];
    float blend = smoothstep(0.8, 1.0, (d - start) / max(end - start, 1e-3));
    if (blend > 0.0) {
        vec3 q = vWorldPos + N * (0.03 + 0.03 * float(c + 1)) * (1.5 - NdotL) + L * 0.02;
        vis = mix(vis, c + 1 < 4 ? cascadeShadow(c + 1, q, 0.0005) : 1.0, blend);
    }
    return vis;
}

// The sun's light through a thin part to `p` (ears, fingers, from behind), each channel fading over `widths` (m): by
// how far it went through, from where it went in (the sun's cascade's nearest depth there; a lit face: none), averaged
// over four taps a texel apart (where that distance drops to nothing within a texel, at what the sun sees as the edge,
// a gradient, not the texels' blocks). Beyond the cascades, or in a split-screen view: none.
vec3 sunTransmission(vec3 p, vec3 widths) {
    if (frame.sunDir.w < 0.5 || frame.viewport.w > 0.5) return vec3(0.0);
    float d = dot(p - frame.camPos.xyz, frame.camForward.xyz);
    int c = 0;
    while (c < 4 && d > frame.cascadeSplits[c]) ++c;
    if (c >= 4) return vec3(0.0);
    vec3 proj = (frame.shadowViewProj[c] * vec4(p, 1.0)).xyz;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0) return vec3(0.0);
    vec2 size = vec2(textureSize(cascadeDepths, 0).xy);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        vec2 at = uv + vec2(i == 0 || i == 3 ? -0.75 : 0.75, i < 2 ? -0.75 : 0.75) / size;
        // (the four depths round it interpolated: a plane's depth exactly, not a step per texel, which would stripe it;
        // past an edge, what's behind isn't where it went in)
        vec2 f = fract(at * size - 0.5);
        vec4 g = textureGather(cascadeDepths, vec3(at, float(c)), 0);
        g = mix(g, vec4(min(min(g.x, g.y), min(g.z, g.w))), greaterThan(g, vec4(proj.z)));
        float entry = mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y);
        sum += exp(-max(proj.z - entry, 0.0) * frame.cascadeScale[c].y / widths);
    }
    return sum * 0.25;
}

// Normal mapping without stored tangents: the frame comes from screen-space derivatives (Schueler 2013)
void tangentFrame(vec3 N, out vec3 T, out vec3 B) {
    vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
    vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    T = dp2perp * duv1.x + dp1perp * duv2.x;
    B = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    T *= scale;
    B *= scale;
}
vec3 bend(vec3 N, vec3 T, vec3 B, vec3 n, float strength) {   // (n: a tangent-space normal, from its map)
    n.xy *= strength;
    n.y = -n.y;   // texture v runs down the image; the map's +Y points up it
    return normalize(T * n.x + B * n.y + N * n.z);
}
// The spread of the normals a normal map's filtered sample averages (render/specular_aa.cpp's, the same sums): its
// mips' normals shorten the more their bumps vary (Toksvig); summed over the maps for specular anti-aliasing
float normalMapVariance(float filteredLength, float strength) {
    float len = clamp(filteredLength, 1e-3, 1.0);
    return (1.0 - len) / len * strength * strength;
}
float mapVariance = 0.0;
vec3 perturbNormal(vec3 N, float strength) {
    vec3 T, B;
    tangentFrame(N, T, B);
    vec3 n = texture(normalMap, gUV).xyz * 2.0 - 1.0;
    mapVariance += normalMapVariance(length(n), strength);
    return bend(N, T, B, n, strength);
}

// Parallax occlusion mapping: the view ray stepped down into the height map (white: the surface; black: `depth`
// texture units below it) until it's under the height there, the two last steps' crossing found; the material is
// read where the ray met it. More steps at grazing angles (and at high quality).
vec2 parallaxUv(vec3 N, vec3 V, float depth) {
    vec3 T, B;
    tangentFrame(N, T, B);
    vec3 v = vec3(dot(V, T), dot(V, B), max(dot(V, N), 0.05));   // (along u and v: B is dP/dv itself)
    float most = frame.shadowParams.z > 1.5 ? 32.0 : frame.shadowParams.z > 0.5 ? 16.0 : 8.0;
    float steps = mix(most, 6.0, clamp(v.z, 0.0, 1.0));
    vec2 delta = v.xy / v.z * depth / steps;
    vec2 gx = dFdx(vUV), gy = dFdy(vUV);   // (loops of different lengths: the gradients given, not guessed)
    vec2 uv = vUV;
    float layer = 0.0, below = 1.0 - textureGrad(heightMap, uv, gx, gy).r, lastLayer = 0.0, lastBelow = below;
    for (int i = 0; i < 40 && layer < below && float(i) < steps; ++i) {
        lastLayer = layer;
        lastBelow = below;
        uv -= delta;
        layer += 1.0 / steps;
        below = 1.0 - textureGrad(heightMap, uv, gx, gy).r;
    }
    // (between the last step above and the first below: where the ray crossed the height, linearly)
    float a = below - layer, b = lastBelow - lastLayer;
    float t = clamp(a / min(a - b, -1e-5), 0.0, 1.0);
    return mix(uv, uv + delta, t);
}

// GGX / Trowbridge-Reitz distribution and height-correlated Smith visibility
float distributionGGX(float NdotH, float a) {
    float a2 = a * a, d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float visibilitySmith(float NdotV, float NdotL, float a) {
    float a2 = a * a;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2), gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
vec3 fresnelSchlick(float VdotH, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0); }

// The surface being lit: its base layer, and the layers over it
struct Surface {
    vec3 N, V, base;
    float metallic, rough;
    vec3 f0, energy;      // energy: the specular scaled up for the light that bounces between microfacets
    float coat, coatRough;
    vec3 coatN;           // (the coat follows the surface, not its normal map)
    float sheen;
    vec3 sheenColor;
    float aniso;
    vec3 T, B;            // (anisotropy's directions: along the texture's u and v)
    float translucency;   // thin things: light from behind through them
    float subsurface;     // thick ones: light spread under the surface, past the shadow's edge, in scatterColor
    vec3 scatterColor;
};

// Light reflected towards V from a light of `radiance`: its diffuse part from along L, its highlights from along Ls
// (a light with a shape: the point of it nearest the reflected ray, render/area_light.h), scaled by `highlight` (so
// a broader light's highlight spreads its energy rather than adding to it). Intensities mean "how bright a white
// matte surface facing the light looks", hence the PI. Its specular part (the highlights, the coat's) is left in
// shadeSpecular.
vec3 shadeSpecular = vec3(0.0);
vec3 shadeArea(Surface s, vec3 L, vec3 Ls, vec3 radiance, float highlight) {
    float ndl = dot(s.N, L), NdotL = max(ndl, 0.0), sNdotL = max(dot(s.N, Ls), 0.0);
    float coatNdotL = s.coat > 0.0 ? max(dot(s.coatN, Ls), 0.0) : 0.0;
    shadeSpecular = vec3(0.0);
    if (NdotL <= 0.0 && sNdotL <= 0.0 && coatNdotL <= 0.0 && s.translucency <= 0.0 && s.subsurface <= 0.0) return vec3(0.0);
    vec3 V = s.V, H = normalize(Ls + V);
    float NdotV = max(dot(s.N, V), 1e-4), NdotH = max(dot(s.N, H), 0.0), VdotH = clamp(dot(V, H), 0.0, 1.0);   // (pow of a negative is NaN)
    float a = max(s.rough * s.rough, 0.002);
    vec3 F = fresnelSchlick(VdotH, s.f0);
    float dv;
    if (abs(s.aniso) > 0.01) {   // anisotropic GGX (Burley's roughnesses; Heitz's visibility): stretched highlights
        float at = max(a * (1.0 + s.aniso), 0.002), ab = max(a * (1.0 - s.aniso), 0.002);
        vec3 d = vec3(ab * dot(s.T, H), at * dot(s.B, H), at * ab * NdotH);
        float w2 = at * ab / max(dot(d, d), 1e-12);
        float D = at * ab * w2 * w2 / PI;
        float lv = sNdotL * length(vec3(at * dot(s.T, V), ab * dot(s.B, V), NdotV));
        float ll = NdotV * length(vec3(at * dot(s.T, Ls), ab * dot(s.B, Ls), sNdotL));
        dv = D * 0.5 / max(lv + ll, 1e-5);
    } else {
        dv = distributionGGX(NdotH, a) * visibilitySmith(NdotV, sNdotL, a);
    }
    vec3 spec = dv * F * s.energy * highlight;
    vec3 diffuse = (1.0 - F) * (1.0 - s.metallic) * s.base / PI;
    vec3 lit = vec3(NdotL);
    if (s.subsurface > 0.0) {   // wrapped diffuse: the light reaches past the terminator, reddened (skin's) there
        float w = 0.5 * s.subsurface;
        float wrapped = clamp((ndl + w) / (1.0 + w), 0.0, 1.0);
        lit = mix(lit, wrapped * mix(s.scatterColor, vec3(1.0), clamp(ndl * 2.0, 0.0, 1.0)), s.subsurface);
    }
    vec3 specular = spec * sNdotL;
    vec3 result = diffuse * lit * (1.0 - 0.25 * s.translucency) + specular;
    if (s.translucency > 0.0) {   // thin: light from behind comes through (its colour), and looking towards the light
                                  // through it (grass, whose normals point up, against the sun) it glows
        float back = max(-ndl, 0.0), towards = pow(clamp(dot(-s.V, L), 0.0, 1.0), 4.0);
        result += (1.0 - s.metallic) * s.base / PI * (s.translucency * (0.6 * back + 1.2 * towards));
    }
    if (s.sheen > 0.0) {   // cloth: Estevez & Kulla's "Charlie" sheen (roughness 0.5), Neubelt's visibility
        const float invA = 4.0;   // (1 / 0.5 squared)
        float sin2h = max(1.0 - NdotH * NdotH, 0.0078125);
        float D = (2.0 + invA) * pow(sin2h, invA * 0.5) / (2.0 * PI);
        float vis = 1.0 / max(4.0 * (sNdotL + NdotV - sNdotL * NdotV), 1e-4);
        result = result * (1.0 - 0.25 * s.sheen) + s.sheenColor * (D * vis * sNdotL);
        specular *= 1.0 - 0.25 * s.sheen;
    }
    if (s.coat > 0.0) {   // a clear coat: its own sharp GGX lobe (f0 0.04, Kelemen's visibility), dimming what's under it
        float ca = max(s.coatRough * s.coatRough, 0.002);
        float Fc = (0.04 + 0.96 * pow(1.0 - VdotH, 5.0)) * s.coat;
        float lobe = distributionGGX(max(dot(s.coatN, H), 0.0), ca) * 0.25 / max(VdotH * VdotH, 1e-4) * highlight;
        result = result * (1.0 - Fc) + vec3(lobe * Fc * coatNdotL);
        specular = specular * (1.0 - Fc) + vec3(lobe * Fc * coatNdotL);
    }
    shadeSpecular = specular * radiance * PI;
    return result * radiance * PI;
}
vec3 shade(Surface s, vec3 L, vec3 radiance) { return shadeArea(s, L, L, radiance, 1.0); }

// Lights with a shape (render/area_light.cpp's, the same sums): the point of a segment nearest p; of the segment
// l0..l1 nearest the ray along r; of a sphere round c nearest it; the highlight's scale
vec3 nearestOnSegment(vec3 a, vec3 b, vec3 p) {
    vec3 d = b - a;
    float dd = dot(d, d);
    return a + d * (dd > 1e-12 ? clamp(dot(p - a, d) / dd, 0.0, 1.0) : 0.0);
}
vec3 segmentNearestRay(vec3 l0, vec3 l1, vec3 r) {
    vec3 ld = l1 - l0;
    float rld = dot(r, ld), denom = dot(ld, ld) - rld * rld;
    return l0 + ld * (denom > 1e-8 ? clamp((dot(r, l0) * rld - dot(l0, ld)) / denom, 0.0, 1.0) : 0.5);
}
vec3 sphereNearestRay(vec3 c, vec3 r, float radius) {
    vec3 toRay = r * dot(c, r) - c;
    float len = length(toRay);
    return len > 1e-6 ? c + toRay * min(radius / len, 1.0) : c;
}
vec3 rectNearest(vec3 c, vec3 x, vec3 y, float hx, float hy) {
    return c + x * clamp(dot(-c, x), -hx, hx) + y * clamp(dot(-c, y), -hy, hy);
}
vec3 rectRayPoint(vec3 c, vec3 x, vec3 y, float hx, float hy, vec3 r) {
    vec3 n = cross(x, y);
    float along = dot(r, n), t = abs(along) > 1e-6 ? dot(c, n) / along : -1.0;
    vec3 d = (t > 0.0 ? r * t : r * (length(c) * 4.0)) - c;   // (away or parallel: far along the ray)
    return c + x * clamp(dot(d, x), -hx, hx) + y * clamp(dot(d, y), -hy, hy);
}
float highlightScale(float alpha, float radius, float len, float dist) {
    alpha = max(alpha, 1e-4);
    dist = max(dist, 1e-4);
    float sphere = alpha / min(alpha + radius / (2.0 * dist), 1.0), tube = alpha / min(alpha + len / (2.0 * dist), 1.0);
    return sphere * sphere * tube;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile"): the specular's scale and
// bias (f0 * x + y)
vec2 envDFG(float rough, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
vec3 envBRDF(vec3 f0, float rough, float NdotV) {
    vec2 ab = envDFG(rough, NdotV);
    return f0 * ab.x + ab.y;
}

// Image-based lighting from the sky: what a surface facing `n` gathers from all around (diffuse), and the sky
// reflected along `r`, blurrier the rougher the surface
// Reflection probes: the first whose box holds this point; it takes over from the sky over the last metre inside
float probeWeight(int i) {
    if (frame.probePos[i].w < 0.5) return 0.0;
    vec3 d = min(vWorldPos - frame.probeMin[i].xyz, frame.probeMax[i].xyz - vWorldPos);
    return clamp(min(d.x, min(d.y, d.z)), 0.0, 1.0);
}
int probeAt(out float w) {
    for (int i = 0; i < 4; ++i) {
        w = probeWeight(i);
        if (w > 0.0) return i;
    }
    return -1;
}
// Where to look in the probe's picture: along r, or (box projection) towards where r meets the box's walls
vec3 probeDir(int i, vec3 r) {
    if (frame.probeMax[i].w < 0.5) return r;
    vec3 a = (frame.probeMax[i].xyz - vWorldPos) / r, b = (frame.probeMin[i].xyz - vWorldPos) / r;
    vec3 far = max(a, b);
    float t = min(far.x, min(far.y, far.z));
    return vWorldPos + r * t - frame.probePos[i].xyz;
}
vec3 probeReflection(int i, vec3 r, float rough) {
    vec2 uv = equirect(normalize(probeDir(i, r)));
    float lod = rough * (kRadianceLevels - 1.0);
    if (i == 0) return textureLod(probe0, uv, lod).rgb;
    if (i == 1) return textureLod(probe1, uv, lod).rgb;
    if (i == 2) return textureLod(probe2, uv, lod).rgb;
    return textureLod(probe3, uv, lod).rgb;
}
vec3 probeIrradiance(int i, vec3 n) {
    vec3 d = normalize(n);
    int o = i * 9;
    vec3 e = frame.probeSh[o].rgb * 0.282095 + frame.probeSh[o + 1].rgb * (0.488603 * d.y) +
             frame.probeSh[o + 2].rgb * (0.488603 * d.z) + frame.probeSh[o + 3].rgb * (0.488603 * d.x) +
             frame.probeSh[o + 4].rgb * (1.092548 * d.x * d.y) + frame.probeSh[o + 5].rgb * (1.092548 * d.y * d.z) +
             frame.probeSh[o + 6].rgb * (0.315392 * (3.0 * d.z * d.z - 1.0)) +
             frame.probeSh[o + 7].rgb * (1.092548 * d.x * d.z) + frame.probeSh[o + 8].rgb * (0.546274 * (d.x * d.x - d.y * d.y));
    return max(e, vec3(0.0)) / kPi;
}

// Global illumination: the eight grid probes around this point (nudged off the surface), blended by distance and
// by how much each faces the surface; `weight` says how much of them there is (0: none drawn here yet)
vec3 giIrradiance(vec3 n, out float weight) {
    weight = 0.0;
    float s = frame.giOrigin.w;
    if (s <= 0.0) return vec3(0.0);
    ivec3 dims = ivec3(frame.giDims.xyz);
    vec3 g = (vWorldPos + n * (0.25 * s) - frame.giOrigin.xyz) / s;
    ivec3 base = ivec3(floor(g));
    if (any(lessThan(base, ivec3(0))) || any(greaterThanEqual(base + 1, dims))) return vec3(0.0);
    vec3 f = g - vec3(base);
    vec3 d = normalize(n);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 cell = ivec3(frame.giCell.xyz) + base + o;
        ivec3 wrapped = ((cell % dims) + dims) % dims;
        int slot = (wrapped.x * dims.y + wrapped.y) * dims.z + wrapped.z, at = slot * 10;
        vec4 pos = giProbes[at];
        vec3 expected = vec3(cell) * s;
        if (pos.w < 0.5 || distance(pos.xyz, expected) > 0.01 * s) continue;   // (not drawn for this cell yet)
        vec3 t = mix(1.0 - f, f, vec3(o));
        float w = t.x * t.y * t.z;
        vec3 toProbe = normalize(pos.xyz - vWorldPos + d * 1e-4);
        float facing = (dot(toProbe, d) + 1.0) * 0.5;
        w *= facing * facing + 0.05;   // (probes behind the surface count little)
        vec3 e = giProbes[at + 1].rgb * 0.282095 + giProbes[at + 2].rgb * (0.488603 * d.y) +
                 giProbes[at + 3].rgb * (0.488603 * d.z) + giProbes[at + 4].rgb * (0.488603 * d.x) +
                 giProbes[at + 5].rgb * (1.092548 * d.x * d.y) + giProbes[at + 6].rgb * (1.092548 * d.y * d.z) +
                 giProbes[at + 7].rgb * (0.315392 * (3.0 * d.z * d.z - 1.0)) +
                 giProbes[at + 8].rgb * (1.092548 * d.x * d.z) + giProbes[at + 9].rgb * (0.546274 * (d.x * d.x - d.y * d.y));
        sum += max(e, vec3(0.0)) / kPi * w;
        total += w;
    }
    if (total <= 1e-4) return vec3(0.0);
    weight = clamp(total * 4.0, 0.0, 1.0);   // (fades out where only stray probes count)
    return sum / total * frame.giDims.w;
}

vec3 skyIrradianceAt(vec3 n) {
    if (frame.envParams.x > 0.5) return skyIrradiance(n) * frame.ambient.rgb;
    if (frame.atmosphere.y > 0.5) {   // the physical sky's, as harmonics (renderer_sky.cpp: unturned, its own brightness)
        vec3 d = normalize(n);
        vec3 e = frame.sh[0].rgb * 0.282095 + frame.sh[1].rgb * (0.488603 * d.y) + frame.sh[2].rgb * (0.488603 * d.z) +
                 frame.sh[3].rgb * (0.488603 * d.x) + frame.sh[4].rgb * (1.092548 * d.x * d.y) +
                 frame.sh[5].rgb * (1.092548 * d.y * d.z) + frame.sh[6].rgb * (0.315392 * (3.0 * d.z * d.z - 1.0)) +
                 frame.sh[7].rgb * (1.092548 * d.x * d.z) + frame.sh[8].rgb * (0.546274 * (d.x * d.x - d.y * d.y));
        return max(e, vec3(0.0)) / kPi * frame.ambient.rgb;
    }
    vec3 above = mix(frame.fogColor.rgb, frame.zenith.rgb, 0.6), below = frame.ground.rgb;
    return mix(below, above, n.y * 0.5 + 0.5) * frame.ambient.rgb;
}
// (probes' light is scaled by the ambient strength like the sky's: indirect light the same strength inside and out)
vec3 irradiance(vec3 n) {
    float w;
    int p = probeAt(w);
    vec3 sky = skyIrradianceAt(n);
    float gw;
    vec3 gi = giIrradiance(n, gw);   // (light from the sky and bounced, where the grid has it)
    sky = mix(sky, gi * frame.ambient.rgb, gw);
    return p < 0 ? sky : mix(sky, probeIrradiance(p, n) * frame.ambient.rgb, w);
}
vec3 skyEnvironment(vec3 r, float rough) {
    if (frame.envParams.x > 0.5) return skyReflection(r, rough) * frame.ambient.rgb;
    vec3 sharp = skyColor(r, false) * frame.ambient.rgb;
    return mix(sharp, skyIrradianceAt(r), clamp(rough * rough * 1.5, 0.0, 1.0));
}
vec3 environment(vec3 r, float rough) {
    float w;
    int p = probeAt(w);
    vec3 sky = skyEnvironment(r, rough);
    vec3 env = p < 0 ? sky : mix(sky, probeReflection(p, r, rough) * frame.ambient.rgb, w);
    if (frame.prevCamPos.w > 0.5 && rough < 0.8) {   // smooth enough: what's on screen, where the ray finds it
        // (traced at half size beforehand for opaque surfaces, else here; rougher: blurrier, handing over to the
        // probes)
        vec4 s = halfTraced ? tracedReflection : traceReflection(vWorldPos, normalize(r), rough, gl_FragCoord.xy);
        env = mix(env, s.rgb, s.a * smoothstep(0.8, 0.3, rough));
    }
    return env;
}

// The volumetric fog in front of a point at `depth` along the view: the four cell columns around it, bilinearly, each
// weighed down where the surface it sees is at another depth (a column through a building next to this street pixel:
// its cells inside the building would darken or light the street's edge, a cell wide)
vec4 fogAt(vec2 uv, float depth) {
    float z = fogVolumeZ(depth);
    vec2 cell = uv * vec2(kFogCells.xy) - 0.5;
    ivec2 c0 = ivec2(floor(cell));
    vec2 f = cell - vec2(c0);
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 o = ivec2(i & 1, i >> 1);
        ivec2 c = clamp(c0 + o, ivec2(0), kFogCells.xy - 1);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float surface = texelFetch(fogSurface, c, 0).r;   // (0: not known this frame)
        float off = surface > 0.0 ? abs(surface - depth) / max(depth, 1e-3) : 0.0;
        w *= 1e-3 + 1.0 / (1.0 + off * off * 400.0);   // (5 % nearer or further: half the weight)
        sum += texture(fogVolume, vec3((vec2(c) + 0.5) / vec2(kFogCells.xy), z)) * w;
        total += w;
    }
    return sum / max(total, 1e-6);
}

// The light cluster a point is in (the decals are binned into the same ones)
uint clusterAt(vec3 p) {
    vec4 c = frame.viewProj * vec4(p, 1.0);
    vec2 uv = clamp(c.xy / c.w * 0.5 + 0.5, vec2(0.0), vec2(0.9999));
    float depth = dot(p - frame.camPos.xyz, frame.camForward.xyz);
    int slice = depth <= frame.clusterDepth.x ? 0 : int(log(depth / frame.clusterDepth.x) * frame.clusterDepth.y);
    slice = clamp(slice, 0, int(frame.clusterGrid.z) - 1);
    ivec2 tile = ivec2(uv * frame.clusterGrid.xy);
    return uint((slice * int(frame.clusterGrid.y) + tile.y) * int(frame.clusterGrid.x) + tile.x);
}

// A decal's texture and normal map by slot (a switch: each case a constant index, so the slot may differ from pixel
// to pixel without dynamic indexing)
vec4 decalColour(int slot, vec2 uv, vec2 gx, vec2 gy) {
    switch (slot) {
    case 0: return textureGrad(decalTextures[0], uv, gx, gy);
    case 1: return textureGrad(decalTextures[1], uv, gx, gy);
    case 2: return textureGrad(decalTextures[2], uv, gx, gy);
    case 3: return textureGrad(decalTextures[3], uv, gx, gy);
    case 4: return textureGrad(decalTextures[4], uv, gx, gy);
    case 5: return textureGrad(decalTextures[5], uv, gx, gy);
    case 6: return textureGrad(decalTextures[6], uv, gx, gy);
    case 7: return textureGrad(decalTextures[7], uv, gx, gy);
    case 8: return textureGrad(decalTextures[8], uv, gx, gy);
    case 9: return textureGrad(decalTextures[9], uv, gx, gy);
    case 10: return textureGrad(decalTextures[10], uv, gx, gy);
    case 11: return textureGrad(decalTextures[11], uv, gx, gy);
    case 12: return textureGrad(decalTextures[12], uv, gx, gy);
    case 13: return textureGrad(decalTextures[13], uv, gx, gy);
    case 14: return textureGrad(decalTextures[14], uv, gx, gy);
    default: return textureGrad(decalTextures[15], uv, gx, gy);
    }
}
vec3 decalNormal(int slot, vec2 uv, vec2 gx, vec2 gy) {
    vec4 t;
    switch (slot) {
    case 0: t = textureGrad(decalNormals[0], uv, gx, gy); break;
    case 1: t = textureGrad(decalNormals[1], uv, gx, gy); break;
    case 2: t = textureGrad(decalNormals[2], uv, gx, gy); break;
    case 3: t = textureGrad(decalNormals[3], uv, gx, gy); break;
    case 4: t = textureGrad(decalNormals[4], uv, gx, gy); break;
    case 5: t = textureGrad(decalNormals[5], uv, gx, gy); break;
    case 6: t = textureGrad(decalNormals[6], uv, gx, gy); break;
    default: t = textureGrad(decalNormals[7], uv, gx, gy); break;
    }
    return t.xyz * 2.0 - 1.0;
}

// Weather: value noise (where puddles form) and raindrops' rings on standing water
float hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float valueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}
// The slope of the water's surface at p (metres, on the ground) from rings spreading where drops fell: one drop per
// 0.4 m cell at a time, each at its own place and moment, its ring growing and fading over its life
vec2 rainRings(vec2 p, float t, float rain) {
    vec2 q = p * 2.5, cell = floor(q), slope = vec2(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 c = cell + vec2(x, y);
            float seed = hash12(c), life = fract(t * (0.8 + 0.6 * seed) + seed * 7.0);
            if (hash12(c + 17.0) > rain) continue;   // (lighter rain: fewer drops)
            vec2 at = c + 0.5 + (vec2(hash12(c + 3.1), hash12(c + 5.7)) - 0.5) * 0.7;
            vec2 d = q - at;
            float r = length(d), x0 = r - life * 0.9;
            float ring = exp(-x0 * x0 * 90.0) * (1.0 - life) * (1.0 - life);
            slope += d / max(r, 1e-4) * ring * cos(x0 * 30.0);
        }
    return slope * 0.6;
}

// Caustics: light focused by the waves onto what's under the water, bright lines where cells of an animated
// cellular pattern meet (two scales); 1 where there's no water above, else its pattern fading with the depth
float cellEdges(vec2 p, float t) {
    vec2 cell = floor(p), f = fract(p);
    float d1 = 8.0, d2 = 8.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 c = cell + vec2(x, y);
            vec2 o = 0.5 + 0.4 * sin(t * 0.9 + 6.2831 * vec2(hash12(c), hash12(c + 19.1)));
            vec2 r = vec2(x, y) + o - f;
            float d = dot(r, r);
            if (d < d1) {
                d2 = d1;
                d1 = d;
            } else if (d < d2) {
                d2 = d;
            }
        }
    return pow(1.0 - smoothstep(0.0, 0.3, sqrt(d2) - sqrt(d1)), 3.0);
}
float caustics(vec3 pos) {
    int count = int(frame.waterInfo.y + 0.5);
    for (int i = 0; i < count; ++i) {
        vec4 b = frame.waterBox[i];
        float level = frame.waterLevel[i].x, below = level - pos.y;
        if (pos.x < b.x || pos.z < b.y || pos.x > b.z || pos.z > b.w || below < 0.02 || below > frame.waterLevel[i].y + 1.0)
            continue;
        vec3 L = normalize(-frame.sunDir.xyz);
        vec2 q = pos.xz + L.xz / max(L.y, 0.2) * below;   // (where the sun's ray to here came through the surface)
        float t = frame.time.x;
        float c = cellEdges(q * 0.9, t) * 0.6 + cellEdges(q * 1.7 + 3.7, t * 1.3) * 0.4;
        return mix(1.0, 0.55 + c * 2.2, exp(-below * 0.35));
    }
    return 1.0;
}

// A terrain's colour: its four layers (the material set's four textures, sRGB) mixed by the vertex weights, each
// tinted and tiled by a column of its layers' matrix (joints[-params.w - 2]; none: white, 4 m tiles)
vec3 terrainColor() {
    vec4 w = max(vSplat, vec4(0.0));
    w /= max(w.x + w.y + w.z + w.w, 1e-4);
    int at = int(-vParams.w + 0.5) - 2;
    mat4 L = at >= 0 ? joints[at] : mat4(vec4(1.0, 1.0, 1.0, 0.25), vec4(1.0, 1.0, 1.0, 0.25), vec4(1.0, 1.0, 1.0, 0.25),
                                         vec4(1.0, 1.0, 1.0, 0.25));
    return w.x * texture(albedoMap, vUV * L[0].w).rgb * L[0].rgb + w.y * texture(normalMap, vUV * L[1].w).rgb * L[1].rgb +
           w.z * texture(metalRoughMap, vUV * L[2].w).rgb * L[2].rgb + w.w * texture(emissiveMap, vUV * L[3].w).rgb * L[3].rgb;
}

// The sunlight the clouds let through to a point: from the volumetric clouds' shadow map where they're marched (the
// ray towards the sun from here meets its plane, height 0, there), else (or outside it) the flat layer's
float cloudsOver(vec3 pos) {
    if (frame.cloudShadowArea.w > 0.5) {
        vec3 L = normalize(frame.skyToSun.xyz);
        vec2 uv = (pos.xz - L.xz / max(L.y, 0.02) * pos.y - frame.cloudShadowArea.xy) / frame.cloudShadowArea.z;
        if (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) return textureLod(cloudShadowMap, uv, 0.0).r;
    }
    return cloudShadow(pos);
}

// A terrain's bumps: its layers' normal maps mixed by the same weights, each tiled as its layer and scaled by its
// strength (column k's x of the matrix after the tints; 0: none, not read), the mix's spread for specular AA
vec3 terrainNormal(vec3 N) {
    int at = int(-vParams.w + 0.5) - 2;
    if (at < 0) return N;
    mat4 L = joints[at], S = joints[at + 1];
    if (S[0].x + S[1].x + S[2].x + S[3].x <= 0.0) return N;
    vec4 w = max(vSplat, vec4(0.0));
    w /= max(w.x + w.y + w.z + w.w, 1e-4);
    vec3 n = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        vec3 m = vec3(0.0, 0.0, 1.0);
        if (S[k].x > 0.0) {   // (the same for the whole draw)
            m = texture(layerNormals[k], vUV * L[k].w).xyz * 2.0 - 1.0;
            mapVariance += w[k] * normalMapVariance(length(m), S[k].x);
            m.xy *= S[k].x;
        }
        n += w[k] * m;
    }
    vec3 T, B;
    tangentFrame(N, T, B);
    return bend(N, T, B, n, 1.0);
}

void main() {
    // Texture streaming: the finest level this texture is needed at, reported by one pixel in sixteen (a different
    // one each frame), a level finer than its footprint (anisotropic filtering reaches further)
    vec2 texels = vUV * exp2(vStream.y);
    vec2 tdx = dFdx(texels), tdy = dFdy(texels);
    if (vStream.x >= 0.0 && ((int(gl_FragCoord.x) + 4 * int(gl_FragCoord.y) + int(frame.time.y)) & 15) == 0) {
        float lod = 0.5 * log2(max(max(dot(tdx, tdx), dot(tdy, tdy)), 1e-8)) - 1.0;
        atomicMin(wantedLevel[uint(vStream.x)], uint(clamp(floor(lod), 0.0, 15.0)));
    }
    vec3 base = vColor.rgb;
    float alpha = vColor.a;
    bool water = vParams.x > 2.5;   // (water: shaders/water.glsl moved its grid; its waves in vEmissive)
    bool terrain = vParams.x > 1.5 && !water;
    if (terrain && vHole > 0.001) discard;   // a hole in the ground
    gUV = vUV;
    if (vSurface.x > 0.0 && !terrain && !water) {   // parallax occlusion: the material read deeper in
        vec3 n0 = normalize(vNormal);
        if (!gl_FrontFacing && vParams.x > -0.5) n0 = -n0;
        gUV = parallaxUv(n0, normalize(frame.camPos.xyz - vWorldPos), vSurface.x);
    }
    if (terrain) {
        base = terrainColor();
    } else if (vParams.z > 0.5) {
        vec4 t = texture(albedoMap, gUV);
        base *= t.rgb;
        alpha *= t.a;
    }
    int alphaMode = int(vMaterial.w + 0.5);
    if (alphaMode == 1 && alpha < vEmissive.w) discard;   // a hole in a cut-out
    bool ground = vParams.x > 0.5 && !terrain && !water;
    vec4 mr = terrain ? vec4(1.0) : texture(metalRoughMap, gUV);
    float metallic = clamp(vMaterial.x * mr.b, 0.0, 1.0);
    float rough = clamp(vMaterial.y * mr.g, 0.02, 1.0);

    if (ground) {
        // 2m checkerboard plus anti-aliased 10m grid lines; a matte surface
        vec2 cell = floor(vWorldPos.xz / 2.0);
        float checker = mod(cell.x + cell.y, 2.0);
        base = mix(base, base * 0.7, checker);
        vec2 gp = vWorldPos.xz / 10.0;
        vec2 g = abs(fract(gp - 0.5) - 0.5) / fwidth(gp);
        float line = 1.0 - min(min(g.x, g.y), 1.0);
        base = mix(base, vec3(0.8), line * 0.6);
        rough = max(rough, 0.9);
    }

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing && vParams.x > -0.5) N = -N;   // (two-sided surfaces; foliage keeps its normals)
    vec3 surfaceN = N;   // (a clear coat's: smooth over the normal map)
    if (vMaterial.z > 0.0) N = perturbNormal(N, vMaterial.z);
    if (terrain) N = terrainNormal(N);
    if (vSurface.z > 0.0 && !terrain && !water) {   // the detail map: finer bumps over the normal map's
        vec3 T, B;
        tangentFrame(surfaceN, T, B);
        vec3 n = texture(detailMap, gUV * vSurface.y).xyz * 2.0 - 1.0;
        mapVariance += normalMapVariance(length(n), vSurface.z);
        N = bend(N, T, B, n, vSurface.z);
    }
    vec3 waterRefracted = vec3(0.0);   // (water with refraction: what shows through, added after lighting)
    float waterFresnel = 1.0;
    if (water) {
        // Ripples on the waves (small ones, drifting with the current), foam where a crest peaks, and the surface
        // more mirror than window at a glancing angle (Fresnel)
        vec2 p = vWorldPos.xz, flow = vStream.zw;
        float t = vStream.y;
        vec2 slope = vec2(0.0);
        const vec3 kRipples[4] = vec3[](vec3(0.8, 0.6, 1.9), vec3(-0.5, 0.86, 3.1), vec3(0.97, -0.26, 4.7), vec3(-0.71, -0.71, 7.3));
        for (int i = 0; i < 4; ++i) {
            vec2 d = kRipples[i].xy;
            float f = kRipples[i].z;
            slope += d * cos(dot(d, p - flow * t) * f + t * sqrt(9.81 * f)) * (0.06 / sqrt(f));
        }
        N = normalize(N - vec3(slope.x, 0.0, slope.y));
        float crest = clamp(vSplat.x, 0.0, 1.0);
        float foam = smoothstep(0.6, 0.95, crest) * clamp(vEmissive.w, 0.0, 1.0);
        float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(N, normalize(frame.camPos.xyz - vWorldPos)), 0.0), 5.0);
        if (frame.waterInfo.x > 0.5) {
            // Refraction: what's behind (the opaque picture), bent by the surface's slope, less so where it's
            // shallow; dimmed through the water's thickness (the colours its own colour lacks first) and filled
            // with its colour, lit from above; foam along the shore, where it's shallow, broken up and drifting
            vec2 size = vec2(textureSize(refraction, 0));
            vec2 here = gl_FragCoord.xy / size;
            float myZ = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
            vec4 straight = textureLod(refraction, here, 0.0);
            vec2 bent = here + N.xz * min(max(straight.a - myZ, 0.0), 2.0) * 0.06;
            vec4 behind = textureLod(refraction, bent, 0.0);
            if (behind.a < myZ) behind = straight;   // (bent onto something in front of the water: not)
            float thickness = max(behind.a - myZ, 0.0);
            vec3 k = (vec3(1.0) - base) * (0.15 + 1.6 * vColor.a) + 0.02;   // (per metre; murkier: vColor.a higher)
            vec3 through = exp(-k * thickness);
            vec3 body = base * (irradiance(vec3(0.0, 1.0, 0.0)) + frame.sunColor.rgb * max(-frame.sunDir.y, 0.0) * 0.15);
            float shore = (1.0 - smoothstep(0.0, 0.5, thickness)) *
                          smoothstep(0.35, 0.75, valueNoise(p * 3.0 - flow * t + vec2(t * 0.25, 0.0)));
            foam = max(foam, shore);
            waterRefracted = (behind.rgb * through + body * (1.0 - through)) * (1.0 - foam);
            waterFresnel = fresnel;
            if (!gl_FrontFacing) {   // seen from below: what's above through Snell's window (48.6 degrees), the
                                     // water's own colour mirrored beyond it (total internal reflection)
                float c = abs(dot(normalize(frame.camPos.xyz - vWorldPos), N));
                waterRefracted = mix(body * 0.5, straight.rgb, smoothstep(0.6, 0.7, c)) * (1.0 - foam);
                waterFresnel = 0.0;
            }
            base = vec3(0.85) * foam;   // (only its foam is lit as a surface; the rest is what shows through)
            alpha = 1.0;
        } else {
            base = mix(base, vec3(0.85), foam * 0.7);
            alpha = clamp(mix(alpha, 1.0, fresnel) + foam * 0.5, 0.0, 1.0);
        }
        rough = mix(rough, 0.6, foam);
        metallic = 0.0;
    }
    // Decals: each box the point is in (nearest first, later ones over earlier; the main view's: those of its
    // cluster) paints its colour (its texture's, where its alpha is), sets the roughness, bends the normal by its
    // normal map and wets the surface; fading at the box's top and bottom and on surfaces turned away from it
    float decalWet = 0.0;
    vec3 decalGlow = vec3(0.0);   // (the light glowing decals give off: over the lit surface, later ones over earlier)
    if (!water && frame.weather.w > 0.5) {
        vec3 dpdx = dFdx(vWorldPos), dpdy = dFdy(vWorldPos);   // (before any branch: for the textures' levels)
        uint first = 0u, count = uint(frame.weather.w + 0.5);
        bool clustered = frame.clusterGrid.w > 0.5;
        if (clustered) {
            uint cluster = clusterAt(vWorldPos);
            first = uint(frame.clusterGrid.x * frame.clusterGrid.y * frame.clusterGrid.z) * 2u + decalClusters[cluster * 2u];
            count = decalClusters[cluster * 2u + 1u];
        }
        for (uint k = 0u; k < count; ++k) {
            int i = clustered ? int(decalClusters[first + k]) : int(k);
            vec3 off = vWorldPos - decals[i].sphere.xyz;   // (most are nowhere near: a sphere's test first)
            if (dot(off, off) > decals[i].sphere.w) continue;
            vec3 q = (decals[i].toBox * vec4(vWorldPos, 1.0)).xyz;
            if (any(greaterThan(abs(q), vec3(0.5)))) continue;
            vec4 p = decals[i].params;
            float facing = dot(surfaceN, -decals[i].down.xyz);
            float a = decals[i].color.a * mix(1.0, smoothstep(0.05, 0.5, facing), p.w) * (1.0 - smoothstep(0.35, 0.5, abs(q.y)));
            vec2 uv = vec2(q.x, q.z) + 0.5;
            vec2 gx = (decals[i].toBox * vec4(dpdx, 0.0)).xz, gy = (decals[i].toBox * vec4(dpdy, 0.0)).xz;
            vec4 tex = p.x >= 0.0 ? decalColour(int(p.x), uv, gx, gy) : vec4(1.0);
            float coverage = a * tex.a;
            base = mix(base, tex.rgb * decals[i].color.rgb, coverage);
            decalGlow = mix(decalGlow, tex.rgb * decals[i].color.rgb * decals[i].normal.z, coverage);
            if (p.y >= 0.0) rough = mix(rough, p.y, coverage);
            decalWet = max(decalWet, p.z * coverage);
            if (decals[i].normal.x >= 0.0) {   // (along its box's x and z: the texture's u and v)
                float shown = mix(1.0, smoothstep(0.05, 0.5, facing), p.w) * tex.a * (1.0 - smoothstep(0.35, 0.5, abs(q.y)));
                mat4 m = decals[i].toBox;
                vec3 T = normalize(vec3(m[0][0], m[1][0], m[2][0])), B = normalize(vec3(m[0][2], m[1][2], m[2][2]));
                vec3 n = decalNormal(int(decals[i].normal.x), uv, gx, gy);
                N = normalize(N + (T * n.x - B * n.y) * (decals[i].normal.y * shown));
            }
        }
    }
    // Weather: wet surfaces (those facing up most; not sheltered ones) darker where porous and glossier; standing
    // water in hollows of flat ground (where noise says, and where decals put puddles) a smooth dark mirror, ringed
    // by the rain
    if (!water && (frame.weather.x > 0.0 || decalWet > 0.0)) {
        float exposed = vSurface.w > 0.5 ? 0.0 : 1.0;
        float wet = max(frame.weather.x * exposed * smoothstep(-0.3, 0.7, surfaceN.y), decalWet);
        float flatness = smoothstep(0.9, 0.98, surfaceN.y);
        float lying = valueNoise(vWorldPos.xz * 0.3) * 0.65 + valueNoise(vWorldPos.xz * 1.1) * 0.35;
        float threshold = 1.0 - frame.weather.y * 0.7;
        float puddle = frame.weather.x * exposed * flatness * smoothstep(threshold, threshold + 0.06, lying);
        puddle = max(puddle, smoothstep(0.7, 1.0, decalWet) * flatness);
        float porous = wet * (1.0 - metallic);
        base *= mix(1.0, 0.5, porous * 0.8);
        rough = mix(rough, min(rough, 0.25), wet * 0.85);
        if (puddle > 0.0) {
            base *= mix(1.0, 0.6, puddle);
            rough = mix(rough, 0.03, puddle);
            metallic = mix(metallic, 0.0, puddle);
            vec3 flatN = surfaceN;
            if (frame.weather.z > 0.0) {   // (rain: rings on it)
                vec2 slope = rainRings(vWorldPos.xz, frame.time.x, frame.weather.z);
                flatN = normalize(vec3(-slope.x, 1.0, -slope.y));
            }
            N = normalize(mix(N, flatN, puddle));
        }
    }
    // Specular anti-aliasing (render/specular_aa.cpp's widenedRoughness): where the normal changes from pixel to pixel
    // (Tokuyoshi & Kaplanyan) or its maps' bumps are smaller than a pixel (Toksvig: their mips' shortened normals),
    // the highlight is widened to match, so small shiny bumps and distant wet ground don't sparkle and crawl
    {
        vec3 du = dFdx(N), dv = dFdy(N);
        float variance = 0.25 * (dot(du, du) + dot(dv, dv));
        float a = rough * rough;
        rough = sqrt(sqrt(clamp(a * a + min(2.0 * variance, 0.18) + min(mapVariance, 0.18), 0.0, 1.0)));
    }
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 1e-4);
    halfTraced = frame.trace.x > 0.5 && alphaMode != 2 && !water;   // (see-through things and water: traced here)
    if (halfTraced) readTraced(rough);

    // The surface: its layers (Instance.layers), and the energy the single-bounce specular misses (Fdez-Aguera's
    // multiple-scattering compensation: rough metals keep their brightness)
    Surface s;
    s.N = N;
    s.V = V;
    s.base = base;
    s.metallic = metallic;
    s.rough = rough;
    s.f0 = mix(vec3(0.04), base, metallic);
    vec2 dfg = envDFG(rough, NdotV);
    s.energy = 1.0 + s.f0 * (1.0 / max(dfg.x + dfg.y, 1e-3) - 1.0);
    const bool layered = kLayers && !water && !terrain;
    s.coat = layered ? clamp(vLayers.x, 0.0, 1.0) : 0.0;
    s.coatRough = clamp(vLayers.y, 0.02, 1.0);
    s.coatN = surfaceN;
    s.sheen = layered ? clamp(vLayers.z, 0.0, 1.0) : 0.0;
    s.sheenColor = s.sheen * mix(base, vec3(1.0), 0.5);
    s.aniso = layered ? clamp(vLayers.w, -0.95, 0.95) : 0.0;
    s.T = vec3(1.0, 0.0, 0.0);
    s.B = vec3(0.0, 0.0, 1.0);
    s.translucency = layered ? clamp(vScatter.x, 0.0, 1.0) : 0.0;
    s.subsurface = layered ? clamp(vScatter.y, 0.0, 1.0) : 0.0;
    s.scatterColor = unpackColor(vScatter.z);
    vec3 R = reflect(-V, N);
    if (abs(s.aniso) > 0.01) {   // the texture's u and v across the surface (screen-space derivatives), and the
                                 // reflection bent towards the highlight's stretch
        vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
        vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
        vec3 t = cross(dp2, N) * duv1.x + cross(N, dp1) * duv2.x;
        t = t - N * dot(N, t);
        s.T = dot(t, t) > 1e-20 ? normalize(t) : normalize(cross(N, abs(N.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
        s.B = cross(N, s.T);
        vec3 along = s.aniso >= 0.0 ? s.B : s.T;
        vec3 bent = cross(cross(along, V), along);
        R = reflect(-V, normalize(mix(N, bent, abs(s.aniso) * 0.8)));
    }

    // Ambient: hemisphere light, diffuse plus a reflection of it (so metals aren't black in the shade)
    vec3 f0 = s.f0;
    // Ambient occlusion (before lighting: frame.occlusion.x, with its history) on the indirect light alone: nooks
    // and contacts get less of the sky's, the probes' and the bounced light, never of the sun's or the lamps'; and
    // its bent normal (where the open sky is) turns the sky's and the probes' light towards what's unhidden
    float ambientOcclusion = 1.0;
    vec3 ambientN = N;
    if (frame.occlusion.x > 0.5) {
        vec4 o = textureLod(occlusionMap, gl_FragCoord.xy * 0.5 / vec2(textureSize(occlusionMap, 0)), 0.0);
        ambientOcclusion = o.r;
        if (o.g > 0.0) {   // (the bent normal is the geometry's: its turn put on the mapped normal)
            vec2 e = o.ba * 2.0 - 1.0;
            vec3 b = vec3(e, 1.0 - abs(e.x) - abs(e.y));
            float t = clamp(-b.z, 0.0, 1.0);
            b.xy += vec2(b.x >= 0.0 ? -t : t, b.y >= 0.0 ? -t : t);
            vec3 turned = normalize(N + normalize(b) - surfaceN);
            if (dot(turned, N) > 0.1) ambientN = turned;
        }
    }
    vec3 diffuseLight = irradiance(ambientN);
    if (frame.prevCamForward.w > 0.0 && alphaMode != 2 && !water) {   // (light bounced off what's on screen nearby)
        bool fine = frame.shadowParams.z > 1.5;
        float spin = frame.shadowParams.y > 0.5 ? mod(frame.time.y, 64.0) * 5.588 : 0.0;
        vec4 g = halfTraced ? tracedIrradiance : traceIrradiance(vWorldPos, N, gl_FragCoord.xy, fine ? 2 : 1, fine ? 8 : 6, spin);
        diffuseLight = mix(diffuseLight, g.rgb, g.a * frame.prevCamForward.w);
    }
    if (s.translucency > 0.0) diffuseLight += irradiance(-N) * (0.6 * s.translucency);   // (the sky behind, through it)
    diffuseLight *= ambientOcclusion;
    // Specular occlusion (Lagarde): where things are close all round (a crevice: the AO, or the GI rays' hits), the
    // reflection of the surroundings is mostly hidden too, more so for rougher surfaces and head-on views
    float specularOcclusion = 1.0;
    float ao = ambientOcclusion;
    if (halfTraced && frame.prevCamForward.w > 0.0) ao = min(ao, 1.0 - clamp(tracedIrradiance.a, 0.0, 1.0) * 0.8);
    if (ao < 1.0) specularOcclusion = clamp(pow(NdotV + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
    vec3 specular = (f0 * dfg.x + dfg.y) * s.energy * environment(R, rough) * specularOcclusion;
    vec3 color = base * (1.0 - metallic) * diffuseLight + specular;
    if (s.sheen > 0.0) {   // (cloth's rim of light from all around)
        color = color * (1.0 - 0.25 * s.sheen) + s.sheenColor * irradiance(N) * ambientOcclusion * (0.05 + 0.6 * pow(1.0 - NdotV, 4.0));
        specular *= 1.0 - 0.25 * s.sheen;
    }
    if (s.coat > 0.0) {   // (the coat's own reflection over the rest)
        float cv = max(dot(s.coatN, V), 1e-4);
        vec3 coatEnv = envBRDF(vec3(0.04), s.coatRough, cv) * s.coat;
        vec3 coatLight = coatEnv * environment(reflect(-V, s.coatN), s.coatRough);
        color = color * (1.0 - coatEnv.x) + coatLight;
        specular = specular * (1.0 - coatEnv.x) + coatLight;
    }
    // (a subsurface material's specular, all of it summed here, kept apart for the diffusion: just its diffuse spread)
    const bool keepSpecular = kLayers && s.subsurface > 0.01 && alphaMode != 2 && frame.occlusion.y > 0.5 &&
                              frame.clusterGrid.w > 0.5 && frame.waterInfo.z < 0.5;

    // Sun
    vec3 L = normalize(-frame.sunDir.xyz);
    float vis = dot(N, L) > 0.0 || (s.coat > 0.0 && dot(s.coatN, L) > 0.0) ? sunVisibility(N, L) : 0.0;
    // (lit from behind, through it: the shadow on its lit side; past a soft surface's terminator: a little towards
    // the light, out of its own shadow; and a soft surface's shadows softer)
    if (dot(N, L) <= 0.0 && s.translucency > 0.0) vis = sunVisibility(-N, L);
    else if (dot(N, L) <= 0.0 && s.subsurface > 0.0) vis = sunVisibility(L, L);
    if (s.subsurface > 0.0) vis = mix(vis, sqrt(vis), s.subsurface);
    if (!water && frame.waterInfo.y > 0.5) vis *= caustics(vWorldPos);   // (under water: the waves focus it)
    if (halfTraced && frame.trace.w > 0.5) vis *= tracedContact;   // (the contact the shadow maps miss)
    if (frame.atmosphere.x > 0.5 && frame.atmosphere.w > 0.0) vis *= cloudsOver(vWorldPos);   // (clouds drifting over)
    vec3 sunLight = frame.sunColor.rgb;
    if (kTint && sunTinted() && vis > 0.0) sunLight *= sunTint(vWorldPos, N);   // (through glass, a curtain: coloured)
    color += shade(s, L, sunLight) * vis;
    specular += shadeSpecular * vis;
    vec3 profile = vec3(1.0);   // (how far each channel spreads under the surface: the material's subsurface colour's)
    if (s.subsurface > 0.0) {
        vec3 c = s.scatterColor / max(max(s.scatterColor.r, s.scatterColor.g), max(s.scatterColor.b, 1e-3));
        profile = 0.15 + 0.85 * sqrt(c);
    }
    if (s.subsurface > 0.0 && dot(N, L) < 0.3) {   // through thin parts (ears, fingers) from behind: by how far the
                                                    // light went through (the sun's depth), each channel fading over its width
        vec3 transmitted = 0.6 * sunTransmission(vWorldPos, 0.004 * s.subsurface * profile);
        float facing = clamp(0.3 - dot(N, L), 0.0, 1.0);
        float clouds = frame.atmosphere.x > 0.5 && frame.atmosphere.w > 0.0 ? cloudsOver(vWorldPos) : 1.0;
        color += s.base * (1.0 - metallic) * transmitted * (facing * s.subsurface * clouds) * frame.sunColor.rgb;
    }

    // Point and spot lights: smooth falloff to zero at their range; spots only inside their cone. The main view
    // shades with its cluster's lights; probe views (clusterGrid.w = 0) with every light.
    uint first = 0u, count = uint(frame.ambient.w);
    bool clustered = frame.clusterGrid.w > 0.5;
    if (clustered) {
        vec4 c = frame.viewProj * vec4(vWorldPos, 1.0);
        vec2 uv = clamp(c.xy / c.w * 0.5 + 0.5, vec2(0.0), vec2(0.9999));
        float depth = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
        int slice = depth <= frame.clusterDepth.x ? 0 : int(log(depth / frame.clusterDepth.x) * frame.clusterDepth.y);
        slice = clamp(slice, 0, int(frame.clusterGrid.z) - 1);
        ivec2 tile = ivec2(uv * frame.clusterGrid.xy);
        uint cluster = uint((slice * int(frame.clusterGrid.y) + tile.y) * int(frame.clusterGrid.x) + tile.x);
        uint headers = uint(frame.clusterGrid.x * frame.clusterGrid.y * frame.clusterGrid.z) * 2u;
        first = headers + clusters[cluster * 2u];
        count = clusters[cluster * 2u + 1u];
    }
    for (uint k = 0u; k < count; ++k) {
        uint index = clustered ? clusters[first + k] : k;
        // (out of its reach (a tube's or a rectangle's grown by half its size): skipped before the rest of it is read;
        // most of a cluster's lamps are, and reading each whole was most of their cost)
        vec4 reach = lights[index].posRange;
        vec3 toReach = reach.xyz - vWorldPos;
        if (dot(toReach, toReach) >= reach.w * reach.w) continue;
        Light li = lights[index];
        vec3 d = li.posRange.xyz - vWorldPos;
        bool tube = li.color.w < 0.5 && li.shape.y > 0.0;   // (a tube along dir: lit from its nearest point)
        bool rect = tube && li.shape.w > 0.0;                // (a rectangle: its x along dir, lighting towards params.xzw)
        vec3 tubeEnd = li.dir.xyz * (0.5 * li.shape.y);
        vec3 facing = vec3(li.params.x, li.params.z, li.params.w), rectY = cross(li.dir.xyz, facing);
        vec3 rectNear = vec3(0.0);   // (a rectangle's nearest point, from its middle: its picture's there)
        if (rect) {
            if (dot(vWorldPos - li.posRange.xyz, facing) <= 0.0) continue;   // (behind it: it lights one way)
            d = rectNearest(d, li.dir.xyz, rectY, 0.5 * li.shape.y, 0.5 * li.shape.w);
            rectNear = d - (li.posRange.xyz - vWorldPos);
        } else if (tube) {
            d = nearestOnSegment(li.posRange.xyz - tubeEnd, li.posRange.xyz + tubeEnd, vWorldPos) - vWorldPos;
        }
        float dist = length(d), range = li.shape.z > 0.0 ? li.shape.z : li.posRange.w;
        if (dist >= range) continue;
        vec3 Lp = d / max(dist, 1e-4);
        vec3 lightSide = dot(N, Lp) < 0.0 && s.translucency > 0.0 ? -N : N;   // (the shadow on the side it's lit from)
        float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
        float light = window * window;
        if (rect) light *= max(dot(-Lp, facing), 0.0);   // (a rectangle's light falls off sideways, as a lit panel's)
        int shadowView = int(li.params.y);
        if (li.color.w > 0.5) {   // a spot
            float cosAngle = dot(-Lp, li.dir.xyz);
            light *= smoothstep(li.dir.w, li.params.x, cosAngle);
            if (light <= 0.0) continue;
            if (li.params.z >= 0.0) {   // its cookie, across the cone
                vec3 axis = li.dir.xyz;
                vec3 side = normalize(cross(axis, abs(axis.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
                vec3 up = cross(side, axis);
                vec3 v = -d / max(dot(-d, axis), 1e-4);   // on the plane one metre down the cone
                vec2 cuv = vec2(dot(v, side), -dot(v, up)) / li.params.w * 0.5 + 0.5;
                light *= dot(texture(cookies, vec3(clamp(cuv, 0.0, 1.0), li.params.z)).rgb, vec3(0.3333));
                if (light <= 0.0) continue;
            }
            if (shadowView >= 0) light *= lampShadow(shadowView, vWorldPos + lightSide * 0.04, 0.00015, li.posRange.xyz, li.shape.x);
        } else if (shadowView >= 0) {   // a point light's cube: the face towards this point (+x -x +y -y +z -z)
            vec3 a = vWorldPos - li.posRange.xyz, m = abs(a);   // (from its middle: a tube's shadows too)
            int face = m.x >= m.y && m.x >= m.z ? (a.x > 0.0 ? 0 : 1) : m.y >= m.z ? (a.y > 0.0 ? 2 : 3) : (a.z > 0.0 ? 4 : 5);
            light *= lampShadow(shadowView + face, vWorldPos + lightSide * (0.02 + 0.01 * dist), 0.0002, li.posRange.xyz, li.shape.x);
            shadowView += face;   // (its tint: that face's)
        }
        if (light <= 0.0) continue;
        if (halfTraced && frame.trace.w > 0.5)   // (the lamps' contact shadows, for those that have them)
            for (int c = 0; c < 4; ++c)
                if (float(index) == frame.contactLights[c].w) light *= tracedLampContacts[c];
        vec3 lampLight = li.color.rgb * light;
        if (kTint && shadowView >= 0 && lampsTinted()) lampLight *= lampTint(shadowView, vWorldPos + lightSide * 0.04);
        // (its highlight from its shape: the tube's point nearest the reflected ray, then its bulb's; the point's
        // highlight scaled to keep the light's energy)
        vec3 Ls = Lp;
        float highlight = 1.0;
        if (tube || li.shape.x > 0.0) {
            vec3 R = reflect(-s.V, s.N), toLight = li.posRange.xyz - vWorldPos;
            vec3 c = rect ? rectRayPoint(toLight, li.dir.xyz, rectY, 0.5 * li.shape.y, 0.5 * li.shape.w, R)
                          : tube ? segmentNearestRay(toLight - tubeEnd, toLight + tubeEnd, R) : toLight;
            vec3 p = sphereNearestRay(c, R, li.shape.x);
            Ls = normalize(p);
            float alpha = max(s.rough * s.rough, 0.002), cd = length(c);
            highlight = highlightScale(alpha, li.shape.x, tube ? li.shape.y : 0.0, cd);
            if (rect) highlight *= alpha / min(alpha + li.shape.w / (2.0 * max(cd, 1e-4)), 1.0);   // (its height too)
        }
        vec3 lit = shadeArea(s, Lp, Ls, lampLight, highlight);
        if (rect && li.dir.w >= 0.0) {   // a rectangle's picture (a screen, a sign): the diffuse its colours where it's
                                         // nearest, blurred the further off (the whole of it lights a far wall), the
                                         // highlight where the reflection meets it, blurred by the roughness
            float w = li.shape.y, h = li.shape.w, size = max(w, h);
            vec3 R = reflect(-s.V, s.N), toLight = li.posRange.xyz - vWorldPos;
            vec3 seen = rectRayPoint(toLight, li.dir.xyz, rectY, 0.5 * w, 0.5 * h, R) - toLight;
            // (u along -x: the picture reads the right way round from in front, where it lights; area_light.cpp's
            // pictureUv and pictureLod, the same sums)
            vec2 nearUv = vec2(0.5 - dot(rectNear, li.dir.xyz) / w, 0.5 - dot(rectNear, rectY) / h);
            vec2 seenUv = vec2(0.5 - dot(seen, li.dir.xyz) / w, 0.5 - dot(seen, rectY) / h);
            float nearLod = log2(max(256.0 * clamp(dist / size, 0.0, 1.0), 1.0));
            float seenLod = log2(max(256.0 * clamp(2.0 * s.rough * s.rough * length(seen + toLight) / size, 0.0, 1.0), 1.0));
            vec3 diffuseTint = pow(textureLod(cookies, vec3(clamp(nearUv, 0.0, 1.0), li.dir.w), nearLod).rgb, vec3(2.2));
            vec3 specularTint = pow(textureLod(cookies, vec3(clamp(seenUv, 0.0, 1.0), li.dir.w), seenLod).rgb, vec3(2.2));
            lit = (lit - shadeSpecular) * diffuseTint + shadeSpecular * specularTint;
            shadeSpecular *= specularTint;
        }
        color += lit;
        specular += shadeSpecular;
    }

    if (!terrain && !water) color += vEmissive.rgb * texture(emissiveMap, gUV).rgb;   // (water: its waves there)
    color += decalGlow;   // (in the picture the screen-space GI traces next frame: it lights what's near it too)

    color += waterRefracted * (1.0 - waterFresnel);   // (what the water lets through: less at a glancing angle)
    if (frame.waterInfo.z > 0.5) color = throughWater(color, vWorldPos);   // (the camera under the water)
    if (frame.atmosphere.x > 0.5 && frame.air.g > 0.0) {   // aerial perspective: the air between, lit like the sky
        vec3 v = vWorldPos - frame.camPos.xyz;
        float dist = length(v);
        vec3 through = exp(-frame.air.rgb * dist);
        vec3 d = normalize(vec3(v.x, max(v.y, 0.0), v.z) + vec3(0.0, 1e-3, 0.0));   // (looking down: the horizon's air)
        color = color * through + physicalSky(d) * (1.0 - through);
        specular *= through;
    }
    if (frame.fogParams.x > 0.5) {   // volumetric: what the fog in front lets through, and the light it adds
        vec4 c = frame.viewProj * vec4(vWorldPos, 1.0);
        float depth = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
        vec4 f = fogAt(c.xy / c.w * 0.5 + 0.5, depth);
        color = color * f.a + f.rgb;
        specular *= f.a;
    } else {
        float dist = length(frame.camPos.xyz - vWorldPos);
        float fog = 1.0 - exp(-pow(dist * frame.fogColor.w, 2.0));
        color = mix(color, frame.fogColor.rgb, fog);
        specular *= 1.0 - fog;
    }
    if (keepSpecular) {
        uvec3 p = uvec3(round(clamp(profile, 0.0, 1.0) * vec3(31.0, 63.0, 31.0)));
        imageStore(skinSpecular, ivec2(gl_FragCoord.xy),
                   uvec4(packHalf2x16(vec2(specular.r, 0.0)), packHalf2x16(vec2(specular.g, 0.0)),
                         packHalf2x16(vec2(specular.b, 0.0)), (p.r << 11) | (p.g << 5) | p.b));
    }
    // See-through: premultiplied (the blend adds what's behind, times 1 - alpha); alpha stays the outline marker
    outColor = alphaMode == 2 ? vec4(color * alpha, alpha) : vec4(color, 1.0);
}
