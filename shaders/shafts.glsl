// Light shafts without volumetric fog (renderer_post.cpp): the light clear air scatters towards the eye along each
// view ray, marched through the sun's shadow cascades (so what's in the way casts dark streaks wherever it is, on
// screen or off) and the four nearest lamps' light: spot lights' beams marched (their cones and shadows), point
// lights' halos worked out whole along the ray (the march would miss their bright core) and shaded by their cube's
// shadows where it passes. A forward-leaning phase (Henyey-Greenstein): brightest looking towards the light. The air
// takes away what it scatters too: the light along the way fades with distance, and rgb's a = what's left of the
// picture behind (the sky's own: kept). Half size; the composite adds it, before the exposure. Split-screen views
// march their own rectangle, through their own cascades.
#include "common.glsl"

#ifdef MS
layout(set = 0, binding = 29) uniform sampler2DMS sceneDepth;   // this frame's (multisampled: sample 0)
ivec2 depthSize() { return textureSize(sceneDepth); }
#else
layout(set = 0, binding = 29) uniform sampler2D sceneDepth;
ivec2 depthSize() { return textureSize(sceneDepth, 0); }
#endif
vec4 drawnArea() { return frame.drawnViewport; }   // (the scene's rectangle as drawn: smaller with a render scale)

layout(set = 0, binding = 1) uniform sampler2DArrayShadow shadowCascades;   // the sun's (comparing)
layout(set = 0, binding = 5) uniform sampler2DShadow shadowAtlas;          // spot and point lights' tiles
struct Light {
    vec4 posRange;   // xyz, w = range
    vec4 color;      // rgb = colour * intensity (linear), w = 1: spot
    vec4 dir;        // the direction it shines, w = cos outer
    vec4 params;     // x = cos inner, y = first shadow view (-1: none)
    vec4 shape;      // x = its size (the bulb's radius, m: lamps' soft shadows)
};
layout(std430, set = 0, binding = 14) readonly buffer Lights { Light lights[]; };

layout(location = 0) out vec4 outColor;

float phase(float c, float g) { return (1.0 - g * g) / (4.0 * 3.14159265 * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5)); }

float sunAt(vec3 x) {
    float d = dot(x - frame.camPos.xyz, frame.camForward.xyz);
    int c = 0;
    while (c < 4 && d > frame.cascadeSplits[c]) ++c;
    if (c >= 4) return 1.0;
    vec3 proj = (frame.shadowViewProj[c] * vec4(x, 1.0)).xyz;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0) return 1.0;
    if (frame.viewport.w > 0.5) {   // (a split-screen view's own cascades: tiles of the atlas)
        vec4 rect = frame.shadowRect[c];
        return texture(shadowAtlas, vec3(rect.xy + uv * rect.zw, proj.z - 0.001));
    }
    return texture(shadowCascades, vec4(uv, float(c), proj.z - 0.001));
}
// A lamp's shadow at x (one tap): a spot's tile, or the point light's cube face towards x (+x -x +y -y +z -z)
float lampShadow(Light li, vec3 x) {
    int view = int(li.params.y);
    if (view < 0) return 1.0;
    vec3 a = x - li.posRange.xyz;
    if (li.color.w < 0.5) {
        vec3 m = abs(a);
        view += m.x >= m.y && m.x >= m.z ? (a.x > 0.0 ? 0 : 1) : m.y >= m.z ? (a.y > 0.0 ? 2 : 3) : (a.z > 0.0 ? 4 : 5);
    }
    vec4 clip = frame.shadowViewProj[view] * vec4(x, 1.0);
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0 || proj.z < 0.0) return 1.0;
    vec4 rect = frame.shadowRect[view];
    return texture(shadowAtlas, vec3(rect.xy + uv * rect.zw, proj.z - 0.0002));
}
// A spot's beam at x: its falloff (no closer than a metre: no spikes by the bulb), cone and shadow
vec3 spotAt(Light li, vec3 x, vec3 dir) {
    vec3 d = li.posRange.xyz - x;
    float dist = length(d), range = li.posRange.w;
    if (dist >= range) return vec3(0.0);
    vec3 L = d / max(dist, 1e-4);
    float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
    float light = window * window / max(dist * dist, 1.0);
    light *= smoothstep(li.dir.w, li.params.x, dot(-L, li.dir.xyz));
    if (light <= 0.0) return vec3(0.0);
    return li.color.rgb * light * lampShadow(li, x) * phase(dot(dir, L), 0.4);
}
// The light a ray gathers passing a point light (light_shafts.cpp's halo): the integral of 1 / distance squared
// (softened by `soft`) over the part of [0, len] inside its range, weighed by its window at the closest point
float halo(Light li, vec3 o, vec3 dir, float len, float soft, out float tc) {
    vec3 p = li.posRange.xyz - o;
    float range = li.posRange.w;
    tc = dot(p, dir);
    float h2 = max(dot(p, p) - tc * tc, 0.0);
    if (h2 >= range * range) return 0.0;
    float half_ = sqrt(range * range - h2);
    float t0 = max(tc - half_, 0.0), t1 = min(tc + half_, len);
    if (t1 <= t0) return 0.0;
    float e = sqrt(h2 + soft * soft);
    float window = 1.0 - h2 / (range * range);
    return (atan((t1 - tc) / e) - atan((t0 - tc) / e)) / e * window * window;
}

void main() {
    // (a half-size pixel of the picture: where it is in the depth as drawn, a render scale's smaller)
    vec4 picture = frame.shaftsViewport, area = drawnArea();
    vec2 px = gl_FragCoord.xy * 2.0;
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
    if (frame.shafts.x <= 0.0 || px.x < picture.x || px.y < picture.y || px.x > picture.x + picture.z || px.y > picture.y + picture.w)
        return;
    vec2 inArea = (px - picture.xy) / max(picture.zw, vec2(1.0));
    ivec2 at = clamp(ivec2(area.xy + inArea * area.zw), ivec2(0), depthSize() - 1);
    float depth = texelFetch(sceneDepth, at, 0).r;
    vec4 w = frame.invViewProj * vec4(inArea * 2.0 - 1.0, min(depth, 0.99999), 1.0);
    vec3 end = w.xyz / w.w, ray = end - frame.camPos.xyz;
    float len = min(length(ray), 120.0);
    vec3 dir = ray / max(length(ray), 1e-4);
    // (each pixel starts at its own point along the way, turning every frame: temporal AA smooths the steps)
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + mod(frame.time.y, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
    int steps = int(frame.shafts.w + 0.5), lampCount = int(frame.shafts.y + 0.5);
    float stepLength = len / float(steps), density = frame.shafts.x;   // (scattering per metre)
    // (what the air takes away: the scattering less what goes on forward anyway, the haze's phase leaning that way,
    // (1 - g) of it: a transport extinction)
    float extinction = density * 0.4;
    float sun = 0.0;
    vec3 lamps = vec3(0.0);
    vec4 seen = vec4(0.0), weight = vec4(0.0);   // (the point lights' shadows along the way, weighed by nearness)
    for (int i = 0; i < steps; ++i) {
        float t = len * (float(i) + jitter) / float(steps);
        vec3 x = frame.camPos.xyz + dir * t;
        float through = exp(-extinction * t);   // (what reaches the eye of what scatters here)
        if (frame.sunDir.w > 0.5) sun += sunAt(x) * through;
        for (int k = 0; k < lampCount; ++k) {
            Light li = lights[int(frame.shaftLamps[k])];
            if (li.color.w > 0.5) {
                lamps += spotAt(li, x, dir) * through;
            } else if (li.params.y >= 0.0) {
                vec3 d = li.posRange.xyz - x;
                float w = 1.0 / max(dot(d, d), 0.25);
                if (dot(d, d) < li.posRange.w * li.posRange.w) { seen[k] += lampShadow(li, x) * w; weight[k] += w; }
            }
        }
    }
    for (int k = 0; k < lampCount; ++k) {   // the point lights' halos, whole, shaded by what the march saw of their shadows
        Light li = lights[int(frame.shaftLamps[k])];
        if (li.color.w > 0.5) continue;
        float tc;
        float gathered = halo(li, frame.camPos.xyz, dir, len, 0.3, tc);
        if (gathered <= 0.0) continue;
        float shadow = li.params.y >= 0.0 ? (weight[k] > 0.0 ? seen[k] / weight[k] : 1.0) : 1.0;
        vec3 closest = frame.camPos.xyz + dir * clamp(tc, 0.0, len);
        vec3 L = normalize(li.posRange.xyz - closest + vec3(0.0, 1e-4, 0.0));
        lamps += li.color.rgb * (gathered / stepLength) * shadow * exp(-extinction * clamp(tc, 0.0, len)) * phase(dot(dir, L), 0.4);
    }
    vec3 toSun = normalize(-frame.sunDir.xyz);
    // (the lamps' light twenty times the sun's air: a beam crosses metres where the sun's light crosses the whole
    // view, and a lamp's beam in clear night air is what's wanted to show)
    vec3 light = frame.sunColor.rgb * (sun * phase(dot(dir, toSun), 0.6)) + lamps * 20.0;
    float left = depth >= 0.99999 ? 1.0 : exp(-extinction * length(ray));   // (the sky: its own air already)
    outColor = vec4(light * (density * stepLength), left);
}
