#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// A soft round dot, or a streak (soft across, fading towards its tail), fading where it meets what is behind it
// (soft particles, where the opaque picture was copied). Additive particles output light (blended
// ONE, ONE), others colour over what's behind. Lit ones (rain) take their colour times the light reaching them:
// the sun, the sky's and the point and spot lights of their cluster, scattered mostly onwards (a drop in a beam
// seen against the light shines brightest), without shadows.

struct Light {
    vec4 posRange;   // xyz, w = range
    vec4 color;      // rgb = colour * intensity (linear), w = 1: spot
    vec4 dir;        // spots: the direction it shines, w = cos outer
    vec4 params;     // x = cos inner
};
layout(std430, set = 0, binding = 14) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 15) readonly buffer Clusters { uint clusters[]; };
// The opaque picture and its distances (renderer.cpp copies it before the see-through things): soft particles
layout(set = 0, binding = 26) uniform sampler2D behind;

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vWorld;
layout(location = 3) flat in vec2 vKind;

layout(location = 0) out vec4 outColor;

float scatter(vec3 towardsLight, vec3 towardsEye) {   // (Henyey-Greenstein, g = 0.6, times 4 pi: 1 on average)
    float c = dot(-towardsLight, towardsEye);
    float g = 0.6, d = 1.0 + g * g - 2.0 * g * -c;
    return (1.0 - g * g) / (d * sqrt(d));
}

vec3 lightAt(vec3 p) {
    vec3 eye = normalize(frame.camPos.xyz - p);
    vec3 sum = frame.ambient.rgb * mix(frame.ground.rgb, frame.zenith.rgb, 0.6);
    sum += frame.sunColor.rgb * scatter(normalize(-frame.sunDir.xyz), eye) * 0.25;
    uint first = 0u, count = uint(frame.ambient.w);
    bool clustered = frame.clusterGrid.w > 0.5;
    if (clustered) {
        vec4 c = frame.viewProj * vec4(p, 1.0);
        vec2 uv = clamp(c.xy / c.w * 0.5 + 0.5, vec2(0.0), vec2(0.9999));
        float depth = dot(p - frame.camPos.xyz, frame.camForward.xyz);
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
        vec3 d = li.posRange.xyz - p;
        float dist = length(d), range = li.posRange.w;
        if (dist >= range) continue;
        vec3 L = d / max(dist, 1e-4);
        float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
        float light = window * window;   // (as the lit shader has it)
        if (li.color.w > 0.5) light *= smoothstep(li.dir.w, li.params.x, dot(-L, li.dir.xyz));
        sum += li.color.rgb * light * scatter(L, eye);
    }
    return sum;
}

void main() {
    float a;
    if (vKind.x > 0.5) {   // a streak: soft across, fading from its head (y = 1) to its tail
        if (abs(vCorner.x) >= 1.0) discard;
        a = vColor.a * (1.0 - smoothstep(0.2, 1.0, abs(vCorner.x))) * (0.5 + 0.5 * vCorner.y);
    } else {
        float d = length(vCorner);
        if (d >= 1.0) discard;
        a = vColor.a * (1.0 - smoothstep(0.35, 1.0, d));
    }
    if (frame.waterInfo.x > 0.5) {   // soft: fading out where what's behind is close (no hard line into the ground)
        float sceneZ = texelFetch(behind, ivec2(gl_FragCoord.xy), 0).a;
        float myZ = dot(vWorld - frame.camPos.xyz, frame.camForward.xyz);
        a *= clamp((sceneZ - myZ) / 0.35, 0.0, 1.0);
        if (a <= 0.0) discard;
    }
    vec3 rgb = vKind.y > 0.5 ? vColor.rgb * lightAt(vWorld) : vColor.rgb;
    outColor = vec4(rgb * a, a);   // premultiplied: the blend states take it from here
}
