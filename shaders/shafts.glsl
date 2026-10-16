// Light shafts without volumetric fog (renderer_post.cpp): the light clear air scatters towards the eye along each
// view ray, marched through the sun's shadow cascades (so what's in the way casts dark streaks wherever it is, on
// screen or off) and up to two shadowed spot lights' beams (their cones and shadows), with a forward-leaning phase
// (Henyey-Greenstein): brightest looking towards the light. Half size; the composite adds it, before the exposure.
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
    return texture(shadowCascades, vec4(uv, float(c), proj.z - 0.001));
}
vec3 lampAt(int index, vec3 x, vec3 dir) {
    Light li = lights[index];
    vec3 d = li.posRange.xyz - x;
    float dist = length(d), range = li.posRange.w;
    if (dist >= range) return vec3(0.0);
    vec3 L = d / max(dist, 1e-4);
    float window = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
    float light = window * window / max(dist * dist, 1.0);   // (no closer than a metre: no spikes by the bulb)
    light *= smoothstep(li.dir.w, li.params.x, dot(-L, li.dir.xyz));
    if (light <= 0.0) return vec3(0.0);
    int view = int(li.params.y);
    if (view >= 0) {   // (its shadow: one tap)
        vec4 clip = frame.shadowViewProj[view] * vec4(x, 1.0);
        vec3 proj = clip.xyz / clip.w;
        vec2 uv = proj.xy * 0.5 + 0.5;
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && proj.z <= 1.0 && proj.z >= 0.0) {
            vec4 rect = frame.shadowRect[view];
            light *= texture(shadowAtlas, vec3(rect.xy + uv * rect.zw, proj.z - 0.0002));
        }
    }
    return li.color.rgb * light * phase(dot(dir, L), 0.4);
}

void main() {
    // (a half-size pixel of the picture: where it is in the depth as drawn, a render scale's smaller)
    vec4 picture = frame.shaftsViewport, area = drawnArea();
    vec2 px = gl_FragCoord.xy * 2.0;
    outColor = vec4(0.0);
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
    int steps = int(frame.shafts.w + 0.5);
    float sun = 0.0;
    vec3 lamps = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 x = frame.camPos.xyz + dir * (len * (float(i) + jitter) / float(steps));
        if (frame.sunDir.w > 0.5) sun += sunAt(x);
        if (frame.shafts.y >= 0.0) lamps += lampAt(int(frame.shafts.y), x, dir);
        if (frame.shafts.z >= 0.0) lamps += lampAt(int(frame.shafts.z), x, dir);
    }
    float stepLength = len / float(steps), density = frame.shafts.x;   // (scattering per metre)
    vec3 toSun = normalize(-frame.sunDir.xyz);
    // (the lamps' beams twenty times the sun's air: a beam crosses metres where the sun's light crosses the whole
    // view, and a lamp's beam in clear night air is what's wanted to show)
    vec3 light = frame.sunColor.rgb * (sun * phase(dot(dir, toSun), 0.6)) + lamps * 20.0;
    outColor = vec4(light * (density * stepLength), 1.0);
}
