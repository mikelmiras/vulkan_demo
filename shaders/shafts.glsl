// Light shafts without volumetric fog (god rays: Mitchell, "Volumetric Light Scattering as a Post-Process", GPU
// Gems 3): from each pixel (half size) a march towards the sun's place on screen, gathering how much of the way is
// sky (the sun's light comes through there; things in the way cast dark streaks), weighed less the further it goes;
// brightest near the sun. The composite adds it to the picture, before the exposure.
layout(push_constant) uniform Shafts {
    vec4 a;   // xy = the sun on screen (full-size pixels), z = strength (with the sun's fade off screen), w = the
              // share of the way towards the sun a march covers
    vec4 b;   // rgb = the sun's light (linear), w = the frame (the march's start turns)
    vec4 c;   // the viewport: x, y, width, height (pixels)
    vec4 d;   // x = render scale (the depth drawn that much smaller, in the viewport's corner)
} pc;
#ifdef MULTISAMPLED
layout(set = 0, binding = 0) uniform sampler2DMS depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
ivec2 depthSize() { return textureSize(depthMap); }
#else
layout(set = 0, binding = 0) uniform sampler2D depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
ivec2 depthSize() { return textureSize(depthMap, 0); }
#endif

layout(location = 0) out vec4 outColor;

// (a full-size pixel of the picture: where it is in the depth drawn, a render scale's smaller)
ivec2 drawn(vec2 px) {
    vec2 p = pc.d.x > 0.0 && pc.d.x < 0.999 ? pc.c.xy + (px - pc.c.xy) * pc.d.x : px;
    return clamp(ivec2(p), ivec2(0), depthSize() - 1);
}

void main() {
    vec2 px = gl_FragCoord.xy * 2.0;
    if (px.x < pc.c.x || px.y < pc.c.y || px.x > pc.c.x + pc.c.z || px.y > pc.c.y + pc.c.w) {
        outColor = vec4(0.0);
        return;
    }
    const int kSteps = 24;
    vec2 toSun = (pc.a.xy - px) * pc.a.w;
    // (each pixel starts at its own point along the way, turning every frame: temporal AA smooths the steps)
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy + mod(pc.b.w, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
    float lit = 0.0, total = 0.0, weight = 1.0;
    for (int i = 0; i < kSteps; ++i) {
        vec2 q = px + toSun * ((float(i) + jitter) / float(kSteps));
        if (q.x < pc.c.x || q.y < pc.c.y || q.x > pc.c.x + pc.c.z || q.y > pc.c.y + pc.c.w) break;
        lit += (depthAt(drawn(q)) >= 0.99999 ? 1.0 : 0.0) * weight;
        total += weight;
        weight *= 0.95;
    }
    float through = total > 0.0 ? lit / total : 0.0;
    float near = exp(-4.0 * length(pc.a.xy - px) / max(pc.c.w, 1.0));   // (in viewport heights from the sun)
    // (over the sky itself, little: it has the sun's glow already; the shafts show against what's in front of it)
    float overSky = depthAt(drawn(px)) >= 0.99999 ? 0.25 : 1.0;
    outColor = vec4(pc.b.rgb * (through * near * overSky * pc.a.z), 1.0);
}
