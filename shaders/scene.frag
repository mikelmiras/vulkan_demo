#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 1) uniform sampler2D shadowMap;   // depth from the directional light
layout(set = 1, binding = 0) uniform sampler2D albedo;      // the material's texture (white if none)

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in vec4 vColor;
layout(location = 4) flat in vec4 vParams;

layout(location = 0) out vec4 outColor;

// 1 = lit, 0 = in shadow; 3x3 samples soften the edge
float sunVisibility(vec3 N, vec3 L) {
    if (frame.sunDir.w < 0.5) return 1.0;
    vec3 p = vWorldPos + N * 0.05 + L * 0.02;   // nudged towards the light: no self-shadowing acne
    vec4 ls = frame.lightViewProj * vec4(p, 1.0);
    vec3 proj = ls.xyz / ls.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0) return 1.0;   // outside the map
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += proj.z - 0.0008 <= texture(shadowMap, uv + vec2(x, y) * texel).r ? 1.0 : 0.0;
    return lit / 9.0;
}

void main() {
    vec3 base = vColor.rgb;
    if (vParams.z > 0.5) base *= texture(albedo, vUV).rgb;
    bool ground = vParams.x > 0.5;

    if (ground) {
        // 2m checkerboard plus anti-aliased 10m grid lines
        vec2 cell = floor(vWorldPos.xz / 2.0);
        float checker = mod(cell.x + cell.y, 2.0);
        base = mix(base, base * 0.7, checker);
        vec2 gp = vWorldPos.xz / 10.0;
        vec2 g = abs(fract(gp - 0.5) - 0.5) / fwidth(gp);
        float line = 1.0 - min(min(g.x, g.y), 1.0);
        base = mix(base, vec3(0.8), line * 0.6);
    }

    vec3 N = normalize(vNormal);
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    float specStrength = ground ? 0.05 : 0.35;

    vec3 skyAmb = vec3(0.45, 0.55, 0.75);
    vec3 groundAmb = vec3(0.20, 0.18, 0.15);
    vec3 color = base * mix(groundAmb, skyAmb, N.y * 0.5 + 0.5) * frame.ambient.rgb;

    // Sun
    vec3 L = normalize(-frame.sunDir.xyz);
    float diff = max(dot(N, L), 0.0);
    if (diff > 0.0) {
        float spec = pow(max(dot(N, normalize(L + V)), 0.0), 48.0) * specStrength;
        color += (base * diff + vec3(spec)) * frame.sunColor.rgb * sunVisibility(N, L);
    }

    // Point lights: smooth falloff to zero at their range
    int count = int(frame.ambient.w);
    for (int i = 0; i < count; ++i) {
        vec3 d = frame.pointPos[i].xyz - vWorldPos;
        float dist = length(d), range = frame.pointPos[i].w;
        if (dist >= range) continue;
        vec3 Lp = d / max(dist, 1e-4);
        float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
        float diffP = max(dot(N, Lp), 0.0);
        float specP = diffP > 0.0 ? pow(max(dot(N, normalize(Lp + V)), 0.0), 48.0) * specStrength : 0.0;
        color += (base * diffP + vec3(specP)) * frame.pointColor[i].rgb * window * window;
    }

    float dist = length(frame.camPos.xyz - vWorldPos);
    float fog = 1.0 - exp(-pow(dist * frame.fogColor.w, 2.0));
    color = mix(color, frame.fogColor.rgb, fog);

    // Swapchain is UNORM (so the UI's sRGB colours stay correct): gamma-encode here. A little noise (half an
    // 8-bit step) hides the banding smooth light falloffs would otherwise show.
    float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    outColor = vec4(pow(color, vec3(1.0 / 2.2)) + noise / 255.0, 1.0);
}
