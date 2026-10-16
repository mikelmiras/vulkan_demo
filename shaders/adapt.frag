#version 450
// Auto-exposure, second pass (into one texel): the exposure (log2 of its factor) that brings the view's brightness
// (metered from a histogram: centre-weighted, the darkest half and the brightest 5% left out, so a few lights or a
// black corner don't swing it) to a key, within its limits, eased from last frame's like an eye adjusting (quicker to a brighter view).
// The key falls with the brightness (Reinhard's automatic key: 0.18 or so by day, far less at night), so a night
// scene stays dark rather than turned into day; the scene's own exposure multiplies it (compensation).

layout(set = 0, binding = 0) uniform sampler2D luminance;   // (its last mip: the view's log2 mean)
layout(set = 0, binding = 1) uniform sampler2D previous;    // last frame's exposure
layout(push_constant) uniform Push {
    vec4 a;   // x = seconds since last frame, y = speed, z = lowest exposure, w = highest (stops)
    vec4 b;   // x = 1: no last frame (start where it should be), y = the mean's mip level
} pc;

layout(location = 0) out vec4 outColor;

// A histogram of the luminance's third level (64 x 36: log2 -16..+16 in 64 bins), each texel weighed up towards the
// middle of the view; the log-average of what's between its 50th and 95th percentiles
float meteredLog() {
    const int kBins = 64;
    const float lo = -16.0, hi = 16.0;
    float bins[kBins];
    for (int i = 0; i < kBins; ++i) bins[i] = 0.0;
    ivec2 size = textureSize(luminance, 2);
    float total = 0.0;
    for (int y = 0; y < size.y; ++y)
        for (int x = 0; x < size.x; ++x) {
            float l = texelFetch(luminance, ivec2(x, y), 2).r;
            vec2 off = (vec2(x, y) + 0.5) / vec2(size) - 0.5;
            float w = 1.0 + 2.0 * exp(-dot(off, off) * 12.0);   // (the middle counts three times the corners)
            int b = clamp(int((l - lo) / (hi - lo) * float(kBins)), 0, kBins - 1);
            bins[b] += w;
            total += w;
        }
    float from = total * 0.5, to = total * 0.95, seen = 0.0, sum = 0.0, weight = 0.0;
    for (int i = 0; i < kBins; ++i) {
        float a = max(from, seen), b = min(to, seen + bins[i]);   // (the part of this bin inside the band)
        if (b > a) {
            sum += (lo + (float(i) + 0.5) / float(kBins) * (hi - lo)) * (b - a);
            weight += b - a;
        }
        seen += bins[i];
    }
    return weight > 0.0 ? sum / weight : textureLod(luminance, vec2(0.5), pc.b.y).r;
}

void main() {
    float meanLog = meteredLog();
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
