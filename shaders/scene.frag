#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Lit objects: metallic-roughness PBR (GGX), sun with shadows, point lights, hemisphere ambient, fog.
// Writes linear HDR colour (alpha 1); the post pass adds bloom, tonemaps and gamma-encodes.

layout(set = 0, binding = 1) uniform sampler2DArray shadowCascades;   // the sun's, 4 layers
layout(set = 0, binding = 5) uniform sampler2DArray spotShadows;      // spot lights', 4 layers
layout(set = 0, binding = 6) uniform sampler2DArray pointShadows;     // point lights', 6 faces each
layout(set = 0, binding = 9) uniform sampler2D probe0;         // reflection probes' radiance (like skyRadiance)
layout(set = 0, binding = 10) uniform sampler2D probe1;
layout(set = 0, binding = 11) uniform sampler2D probe2;
layout(set = 0, binding = 12) uniform sampler2D probe3;
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

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

// 1 = lit, 0 = in shadow, for a point seen by a shadow camera (clip space); 3x3 samples soften the edge
float shadowTest(sampler2DArray map, float layer, vec4 clip, float bias) {
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0 || proj.z < 0.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(map, 0).xy);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += proj.z - bias <= texture(map, vec3(uv + vec2(x, y) * texel, layer)).r ? 1.0 : 0.0;
    return lit / 9.0;
}

// The sun: the cascade covering this distance from the camera (beyond the last one: lit)
float sunVisibility(vec3 N, vec3 L) {
    if (frame.sunDir.w < 0.5) return 1.0;
    float d = dot(vWorldPos - frame.camPos.xyz, frame.camForward.xyz);
    int c = 0;
    while (c < 4 && d > frame.cascadeSplits[c]) ++c;
    if (c >= 4) return 1.0;
    // Nudged along the normal and towards the light (more for the bigger, coarser cascades): no shadow acne
    vec3 p = vWorldPos + N * (0.03 + 0.03 * float(c)) + L * 0.02;
    return shadowTest(shadowCascades, float(c), frame.shadowViewProj[c] * vec4(p, 1.0), 0.0005);
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
    float NdotV = max(dot(N, V), 1e-4), NdotH = max(dot(N, H), 0.0), VdotH = max(dot(V, H), 0.0);
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
    return p < 0 ? sky : mix(sky, probeReflection(p, r, rough) * frame.ambient.rgb, w);
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
    vec3 base = vColor.rgb;
    float alpha = vColor.a;
    bool terrain = vParams.x > 1.5;
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
    if (!gl_FrontFacing) N = -N;   // (two-sided surfaces)
    if (vMaterial.z > 0.0) N = perturbNormal(N, vMaterial.z);
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    // Ambient: hemisphere light, diffuse plus a reflection of it (so metals aren't black in the shade)
    vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 color = base * (1.0 - metallic) * irradiance(N) + envBRDF(f0, rough, NdotV) * environment(reflect(-V, N), rough);

    // Sun
    vec3 L = normalize(-frame.sunDir.xyz);
    float vis = dot(N, L) > 0.0 ? sunVisibility(N, L) : 0.0;
    color += shade(N, V, L, frame.sunColor.rgb, base, metallic, rough) * vis;

    // Point and spot lights: smooth falloff to zero at their range; spots only inside their cone
    int count = int(frame.ambient.w);
    for (int i = 0; i < count; ++i) {
        vec3 d = frame.pointPos[i].xyz - vWorldPos;
        float dist = length(d), range = frame.pointPos[i].w;
        if (dist >= range) continue;
        vec3 Lp = d / max(dist, 1e-4);
        float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
        float light = window * window;
        vec4 spot = frame.pointSpot[i];
        if (spot.y > -1.5) {
            light *= smoothstep(spot.y, spot.x, dot(-Lp, frame.pointDir[i].xyz));
            if (light <= 0.0) continue;
            if (spot.z >= 0.0) {
                vec3 p = vWorldPos + N * 0.04;
                light *= shadowTest(spotShadows, spot.z - 4.0, frame.shadowViewProj[int(spot.z)] * vec4(p, 1.0), 0.00015);
            }
        } else if (spot.z >= 0.0) {   // a point light's cube: the face towards this point (+x -x +y -y +z -z)
            vec3 a = -d, m = abs(a);
            int face = m.x >= m.y && m.x >= m.z ? (a.x > 0.0 ? 0 : 1) : m.y >= m.z ? (a.y > 0.0 ? 2 : 3) : (a.z > 0.0 ? 4 : 5);
            int layer = int(spot.z) + face;
            vec3 p = vWorldPos + N * (0.02 + 0.01 * dist);
            light *= shadowTest(pointShadows, float(layer - 8), frame.shadowViewProj[layer] * vec4(p, 1.0), 0.0002);
            if (light <= 0.0) continue;
        }
        color += shade(N, V, Lp, frame.pointColor[i].rgb * light, base, metallic, rough);
    }

    if (!terrain) color += vEmissive.rgb * texture(emissiveMap, vUV).rgb;

    float dist = length(frame.camPos.xyz - vWorldPos);
    float fog = 1.0 - exp(-pow(dist * frame.fogColor.w, 2.0));
    color = mix(color, frame.fogColor.rgb, fog);
    // See-through: premultiplied (the blend adds what's behind, times 1 - alpha); alpha stays the outline marker
    outColor = alphaMode == 2 ? vec4(color * alpha, alpha) : vec4(color, 1.0);
}
