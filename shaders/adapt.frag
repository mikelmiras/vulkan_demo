#version 450
// Auto-exposure, second pass (into one texel): the exposure (log2 of its factor) that brings the view's mean
// brightness to a key, within its limits, eased from last frame's like an eye adjusting (quicker to a brighter view).
// The key falls with the brightness (Reinhard's automatic key: 0.18 or so by day, far less at night), so a night
// scene stays dark rather than turned into day; the scene's own exposure multiplies it (compensation).

layout(set = 0, binding = 0) uniform sampler2D luminance;   // (its last mip: the view's log2 mean)
layout(set = 0, binding = 1) uniform sampler2D previous;    // last frame's exposure
layout(push_constant) uniform Push {
    vec4 a;   // x = seconds since last frame, y = speed, z = lowest exposure, w = highest (stops)
    vec4 b;   // x = 1: no last frame (start where it should be), y = the mean's mip level
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    float meanLog = textureLod(luminance, vec2(0.5), pc.b.y).r;
    float mean = exp2(meanLog);
    float key = 1.03 - 2.0 / (2.0 + log(mean + 1.0) / log(10.0));
    float target = clamp(log2(max(key, 0.02)) - meanLog, pc.a.z, pc.a.w);
    float before = texelFetch(previous, ivec2(0), 0).r;
    if (pc.b.x > 0.5 || isnan(before) || isinf(before)) {
        outColor = vec4(target, 0.0, 0.0, 1.0);
        return;
    }
    // (towards a darker exposure, a brighter view, three times quicker: the eye closes faster than it opens)
    float rate = pc.a.y * (target < before ? 3.0 : 1.0);
    outColor = vec4(before + (target - before) * (1.0 - exp(-pc.a.x * rate)), 0.0, 0.0, 1.0);
}
