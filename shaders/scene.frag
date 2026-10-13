#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "fog.glsl"
#ifdef EARLY_DEPTH
// (scene_early.frag.spv: after the depth prepass, only the pixel in front is shaded; the depth test before the
// shader, which its texture-streaming writes would otherwise put after it)
layout(early_fragment_tests) in;
#endif
// Lit objects: metallic-roughness PBR (GGX), sun with shadows, point lights, hemisphere ambient, fog.
// Writes linear HDR colour (alpha 1); the post pass adds bloom, tonemaps and gamma-encodes.

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowCascades;   // the sun's, 4 layers (comparing)
layout(set = 0, binding = 5) uniform sampler2DShadow shadowAtlas;          // spot and point lights' (tiles; comparing)
layout(set = 0, binding = 20) uniform sampler2DArray cascadeDepths;        // the sun's again, as depths (PCSS)
layout(set = 0, binding = 21) uniform sampler3D fogVolume;                 // volumetric fog, integrated (fog.glsl)
layout(set = 0, binding = 6) uniform sampler2DArray cookies;          // spot lights' cookies
// Point and spot lights, and the clusters of the view they reach (render/light_clusters.h)
struct Light {
    vec4 posRange;   // xyz, w = range
    vec4 color;      // rgb = colour * intensity (linear), w = 1: spot
    vec4 dir;        // spots: the direction it shines, w = cos outer
    vec4 params;     // x = cos inner, y = first shadow view (-1: none), z = cookie layer (-1: none), w = tan outer
};
layout(std430, set = 0, binding = 14) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 15) readonly buffer Clusters { uint clusters[]; };   // (offset, count) each, then indices
layout(set = 0, binding = 9) uniform sampler2D probe0;         // reflection probes' radiance (like skyRadiance)
layout(set = 0, binding = 10) uniform sampler2D probe1;
layout(set = 0, binding = 11) uniform sampler2D probe2;
layout(set = 0, binding = 12) uniform sampler2D probe3;
// Dynamic GI probes (renderer_gi.cpp): per slot, position (w = 1: drawn), then nine spherical harmonics
layout(std430, set = 0, binding = 17) readonly buffer GiProbes { vec4 giProbes[]; };
// Screen-space reflections: the last frame's picture (rgb) and each pixel's distance along its view (a)
layout(set = 0, binding = 18) uniform sampler2D lastFrame;
// Texture streaming: the finest mip level each texture was wanted at this frame (texture_streaming.h)
layout(std430, set = 0, binding = 19) buffer TextureFeedback { uint wantedLevel[]; };
layout(set = 1, binding = 0) uniform sampler2D albedoMap;      // sRGB view (white if none)
layout(set = 1, binding = 1) uniform sampler2D normalMap;      // tangent space (flat if none)
layout(set = 1, binding = 2) uniform sampler2D metalRoughMap;  // g = roughness, b = metallic (white if none)
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;    // sRGB view (white if none)

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

layout(location = 0) out vec4 outColor;

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
    if (size > 0.0) {
        float search = clamp(size * 20.0 / scale.x, 2.0 * texel, 64.0 * texel);   // (casters up to ~20 m away)
        float sum = 0.0, n = 0.0;
        for (int i = 0; i < 12; ++i) {
            float d = textureLod(cascadeDepths, vec3(uv + rot * kDisk[i] * search, float(c)), 0.0).r;
            if (d < z) { sum += d; n += 1.0; }
        }
        if (n == 0.0) return 1.0;   // (nothing between it and the sun)
        float behind = (z - sum / n) * scale.y;   // metres from the blockers
        radius = clamp(behind * size / scale.x, 1.5 * texel, (frame.shadowParams.y > 0.5 ? 64.0 : 16.0) * texel);
    }
    float lit = 0.0;
    for (int i = 0; i < 16; ++i) lit += cascadeTap(uv + rot * kDisk[i] * radius, float(c), z);
    return lit / 16.0;
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
    float lit = 0.0;
    for (int i = 0; i < 12; ++i)
        lit += atlasTap(clamp(at + rot * kDisk[i] * texel * 1.75, lo, hi), proj.z - bias);
    return lit / 12.0;
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

// Normal mapping without stored tangents: the frame comes from screen-space derivatives (Schueler 2013)
vec3 perturbNormal(vec3 N, float strength) {
    vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
    vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    vec3 n = texture(normalMap, vUV).xyz * 2.0 - 1.0;
    n.xy *= strength;
    n.y = -n.y;   // texture v runs down the image; the map's +Y points up it
    return normalize(T * scale * n.x + B * scale * n.y + N * n.z);
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

// Light reflected towards V from a light of `radiance` along L. Intensities mean "how bright a white matte surface
// facing the light looks", hence the PI.
vec3 shade(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 base, float metallic, float rough) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    vec3 H = normalize(L + V);
    float NdotV = max(dot(N, V), 1e-4), NdotH = max(dot(N, H), 0.0), VdotH = clamp(dot(V, H), 0.0, 1.0);   // (pow of a negative is NaN)
    float a = max(rough * rough, 0.002);
    vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 F = fresnelSchlick(VdotH, f0);
    vec3 spec = distributionGGX(NdotH, a) * visibilitySmith(NdotV, NdotL, a) * F;
    vec3 diffuse = (1.0 - F) * (1.0 - metallic) * base / PI;
    return (diffuse + spec) * radiance * NdotL * PI;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile")
vec3 envBRDF(vec3 f0, float rough, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
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
// Screen-space reflection: marches along r through the last frame's picture (each point placed with its camera)
// until it passes just behind what was drawn there; rgb = what it hit, a = how much to trust it (0: nothing)
vec4 screenReflection(vec3 r) {
    vec2 size = vec2(textureSize(lastFrame, 0));
    vec3 start = vWorldPos + r * 0.05;
    // A point t along the ray, in the last frame's clip space and along its view: both straight lines in t, so
    // projected once here and stepped with additions below
    vec4 clip0 = frame.prevViewProj * vec4(start, 1.0), clipDir = frame.prevViewProj * vec4(r, 0.0);
    float z0 = dot(start - frame.prevCamPos.xyz, frame.prevCamForward.xyz), zDir = dot(r, frame.prevCamForward.xyz);
    vec2 scale = frame.prevViewport.zw * 0.5 / size, offset = (frame.prevViewport.xy + frame.prevViewport.zw * 0.5) / size;
    float t = 0.1, step = 0.12, last = 0.0;
    const float maxDistance = 40.0;
    for (int i = 0; i < 40 && t < maxDistance; ++i) {
        vec4 clip = clip0 + clipDir * t;
        if (clip.w <= 1e-4) break;
        vec2 ndc = clip.xy / clip.w;
        if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) break;   // (off the picture)
        vec2 uv = ndc * scale + offset;
        float sceneZ = textureLod(lastFrame, uv, 0.0).a;
        float rayZ = z0 + zDir * t;
        // (behind it by less than a step, or a little more: thin things aren't missed between two samples)
        if (rayZ > sceneZ && rayZ < sceneZ + max(0.25 + t * 0.04, step * 1.5)) {
            float lo = last, hi = t;   // (the crossing, closer)
            for (int k = 0; k < 5; ++k) {
                float mid = 0.5 * (lo + hi);
                vec4 c = clip0 + clipDir * mid;
                if (c.w <= 1e-4) break;   // (behind the last frame's camera: no point on its picture)
                vec2 u = c.xy / c.w * scale + offset;
                if (z0 + zDir * mid > textureLod(lastFrame, u, 0.0).a) hi = mid;
                else lo = mid;
                uv = u;
            }
            vec2 edge = smoothstep(vec2(1.0), vec2(0.85), abs(ndc));   // (fades out at the picture's edges)
            vec3 seen = textureLod(lastFrame, uv, 0.0).rgb;
            if (any(isnan(seen)) || any(isinf(seen))) return vec4(0.0);
            return vec4(min(seen, vec3(64.0)), edge.x * edge.y * (1.0 - t / maxDistance));
        }
        last = t;
        t += step;
        step *= 1.12;
    }
    return vec4(0.0);
}

vec3 environment(vec3 r, float rough) {
    float w;
    int p = probeAt(w);
    vec3 sky = skyEnvironment(r, rough);
    vec3 env = p < 0 ? sky : mix(sky, probeReflection(p, r, rough) * frame.ambient.rgb, w);
    if (frame.prevCamPos.w > 0.5 && rough < 0.55) {   // smooth enough: what's on screen, where the ray finds it
        vec4 s = screenReflection(normalize(r));
        env = mix(env, s.rgb, s.a * (1.0 - rough / 0.55));
    }
    return env;
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
    bool terrain = vParams.x > 1.5;
    if (terrain && vHole > 0.001) discard;   // a hole in the ground
    if (terrain) {
        base = terrainColor();
    } else if (vParams.z > 0.5) {
        vec4 t = texture(albedoMap, vUV);
        base *= t.rgb;
        alpha *= t.a;
    }
    int alphaMode = int(vMaterial.w + 0.5);
    if (alphaMode == 1 && alpha < vEmissive.w) discard;   // a hole in a cut-out
    bool ground = vParams.x > 0.5 && !terrain;
    vec4 mr = terrain ? vec4(1.0) : texture(metalRoughMap, vUV);
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
    if (vMaterial.z > 0.0) N = perturbNormal(N, vMaterial.z);
    // Specular anti-aliasing (Tokuyoshi & Kaplanyan): where the normal changes within a pixel, the highlight is
    // widened to match, so small shiny bumps and distant wet ground don't sparkle and crawl
    {
        vec3 du = dFdx(N), dv = dFdy(N);
        float variance = 0.25 * (dot(du, du) + dot(dv, dv));
        float a = rough * rough;
        rough = sqrt(sqrt(clamp(a * a + min(2.0 * variance, 0.18), 0.0, 1.0)));
    }
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    // Ambient: hemisphere light, diffuse plus a reflection of it (so metals aren't black in the shade)
    vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 color = base * (1.0 - metallic) * irradiance(N) + envBRDF(f0, rough, NdotV) * environment(reflect(-V, N), rough);

    // Sun
    vec3 L = normalize(-frame.sunDir.xyz);
    float vis = dot(N, L) > 0.0 ? sunVisibility(N, L) : 0.0;
    color += shade(N, V, L, frame.sunColor.rgb, base, metallic, rough) * vis;

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
        Light li = lights[clustered ? clusters[first + k] : k];
        vec3 d = li.posRange.xyz - vWorldPos;
        float dist = length(d), range = li.posRange.w;
        if (dist >= range) continue;
        vec3 Lp = d / max(dist, 1e-4);
        float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
        float light = window * window;
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
            if (shadowView >= 0) light *= atlasShadow(shadowView, vWorldPos + N * 0.04, 0.00015);
        } else if (shadowView >= 0) {   // a point light's cube: the face towards this point (+x -x +y -y +z -z)
            vec3 a = -d, m = abs(a);
            int face = m.x >= m.y && m.x >= m.z ? (a.x > 0.0 ? 0 : 1) : m.y >= m.z ? (a.y > 0.0 ? 2 : 3) : (a.z > 0.0 ? 4 : 5);
            light *= atlasShadow(shadowView + face, vWorldPos + N * (0.02 + 0.01 * dist), 0.0002);
        }
        if (light <= 0.0) continue;
        color += shade(N, V, Lp, li.color.rgb * light, base, metallic, rough);
    }

    if (!terrain) color += vEmissive.rgb * texture(emissiveMap, vUV).rgb;

    if (frame.fogParams.x > 0.5) {   // volumetric: what the fog in front lets through, and the light it adds
        vec4 c = frame.viewProj * vec4(vWorldPos, 1.0);
        float depth = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
        vec4 f = texture(fogVolume, vec3(c.xy / c.w * 0.5 + 0.5, fogVolumeZ(depth)));
        color = color * f.a + f.rgb;
    } else {
        float dist = length(frame.camPos.xyz - vWorldPos);
        float fog = 1.0 - exp(-pow(dist * frame.fogColor.w, 2.0));
        color = mix(color, frame.fogColor.rgb, fog);
    }
    // See-through: premultiplied (the blend adds what's behind, times 1 - alpha); alpha stays the outline marker
    outColor = alphaMode == 2 ? vec4(color * alpha, alpha) : vec4(color, 1.0);
}
