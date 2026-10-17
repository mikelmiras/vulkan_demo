#version 450
// The sky behind everything (the physical sky, a sky image or the gradient), with its clouds marched through, the fog
// in front of it and the water above the camera
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "fog.glsl"

layout(set = 0, binding = 21) uniform sampler3D fogVolume;   // volumetric fog (fog.glsl): the sky is behind all of it

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

// Volumetric clouds (the physical sky's, at medium and high quality): a slab 1500 to 1900 m up marched along the
// view: the flat layer's coverage (cloudDensity, so their shadows on the ground match) given a height profile
// (flat bases, domes over them as tall as they're dense) and its edges eroded by a 3-D noise; each sample lit by the sun through the cloud
// above it (a short march towards the sun: Beer's law), a two-lobed phase (bright looking towards the sun, a
// little back-scattering), its thin edges darker (the "powder" term), and the sky's light from all round.
// rgb = the light they send this way, a = what of the sky behind gets through
const float kCloudBase = 1500.0, kCloudTop = 1900.0;
float noise3(vec3 p) {
    vec3 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    float n000 = skyHash(i), n100 = skyHash(i + vec3(1, 0, 0)), n010 = skyHash(i + vec3(0, 1, 0)), n110 = skyHash(i + vec3(1, 1, 0));
    float n001 = skyHash(i + vec3(0, 0, 1)), n101 = skyHash(i + vec3(1, 0, 1)), n011 = skyHash(i + vec3(0, 1, 1)), n111 = skyHash(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}
float cloudAt(vec3 p, bool detail) {
    float h = (p.y - kCloudBase) / (kCloudTop - kCloudBase);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    float cover = cloudDensity(p.xz);
    if (cover <= 0.0) return 0.0;
    // (render/cloud_shape.cpp's, the same sums: a flat base, as wide as the cloud, and a dome over it: the denser, the
    // taller, rounded as h^2 grows; thin ones
    // stay low and wide, not columns or tents)
    float d = smoothstep(0.0, 0.15, cover - 0.2 - 0.8 * h * h) * smoothstep(0.0, 0.03, h);
    if (detail && d > 0.0) d -= (noise3(p / 350.0 + vec3(0.0, 0.0, frame.time.x * 0.01)) - 0.3) * 0.25;
    return clamp(d, 0.0, 1.0);
}
float henyeyGreenstein(float c, float g) { return (1.0 - g * g) / (4.0 * kPi * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5)); }
vec4 volumetricClouds(vec3 d, float noise) {
    if (d.y <= 0.02) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 o = frame.camPos.xyz;
    float t0 = max((kCloudBase - o.y) / d.y, 0.0), t1 = (kCloudTop - o.y) / d.y;
    if (t1 <= t0) return vec4(0.0, 0.0, 0.0, 1.0);
    bool fine = frame.shadowParams.z > 1.5;
    int steps = fine ? 16 : 10;
    // (4 km of the ray at most: a low ray's long way through the slab is cut short, fading out over its second half
    // so where it's cut doesn't show; the horizon's thinning hides the rest)
    float span = min(t1 - t0, 4000.0), ds = span / float(steps);
    vec3 L = normalize(frame.skyToSun.xyz);
    float c = dot(d, L);
    float phase = mix(henyeyGreenstein(c, 0.6), henyeyGreenstein(c, -0.2), 0.3) * 4.0 * kPi;   // (1: no preference)
    vec3 sun = frame.atmosphere.z > 0.5 ? frame.sunColor.rgb * 0.5 : frame.sunColor.rgb;
    vec3 ambient = (frame.zenith.rgb + frame.fogColor.rgb) * 0.3;
    const float kExtinction = 0.0015;   // (per metre at full density)
    vec3 light = vec3(0.0);
    float through = 1.0;
    for (int i = 0; i < steps && through > 0.02; ++i) {
        float along = (float(i) + noise) * ds;
        vec3 p = o + d * (t0 + along);
        float rho = cloudAt(p, true) * smoothstep(span, span * 0.5, along);
        if (rho <= 0.0) continue;
        // (the sun's light at p, through the cloud above it towards the sun: two steps, coverage alone)
        float above = cloudAt(p + L * 120.0, false) + cloudAt(p + L * 300.0, false);
        float sunThrough = exp(-above * 180.0 * kExtinction);
        float powder = 1.0 - exp(-rho * 6.0);
        vec3 S = sun * (sunThrough * phase * mix(1.0, powder, 0.5)) + ambient * (0.4 + 0.6 * (p.y - kCloudBase) / (kCloudTop - kCloudBase));
        float sigma = rho * kExtinction, stepThrough = exp(-sigma * ds);
        light += through * S * (1.0 - stepThrough);   // (what this step scatters this way, of what reaches it)
        through *= stepThrough;
    }
    float fade = smoothstep(0.02, 0.12, d.y);   // (thinning to the horizon, as the flat layer does)
    return vec4(light * fade, mix(1.0, through, fade));
}

void main() {
    float tanY = frame.camForward.w, aspect = frame.viewport.x / max(frame.viewport.y, 1.0);
    vec3 dir = normalize(frame.camForward.xyz + frame.camRight.xyz * (vNdc.x * tanY * aspect) -
                         frame.camUp.xyz * (vNdc.y * tanY));   // (clip-space y points down)
    // (the physical sky's clouds marched at medium and high quality; else the flat layer)
    bool volumetric = frame.atmosphere.x > 0.5 && frame.envParams.x < 0.5 && frame.atmosphere.w > 0.0 && frame.shadowParams.z > 0.5;
    skyFlatClouds = !volumetric;
    vec3 color = skyColor(dir, true);
    if (volumetric) {
        float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + mod(frame.time.y, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
        vec4 clouds = volumetricClouds(dir, noise);
        color = color * clouds.a + clouds.rgb;
    }
    if (frame.fogParams.x > 0.5) {
        vec4 f = texture(fogVolume, vec3(vNdc * 0.5 + 0.5, 1.0));
        color = color * f.a + f.rgb;
    }
    if (frame.waterInfo.z > 0.5) color = throughWater(color, frame.camPos.xyz + dir * 1e4);   // (from under the water)
    outColor = vec4(color, 1.0);
}
