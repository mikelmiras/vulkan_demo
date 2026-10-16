// The half-size traces (ssr_trace.frag, gi_trace.frag; renderer_trace.cpp): each half-size pixel stands for one of
// its 2 x 2 full-size pixels, a different one each frame, and keeps a history of the others (reprojected by the
// motion vectors, dropped where the depth says it saw something else there)
#ifdef MS
layout(set = 0, binding = 29) uniform sampler2DMS sceneDepth;   // this frame's (the prepass's; multisampled: sample 0)
#else
layout(set = 0, binding = 29) uniform sampler2D sceneDepth;     // this frame's (the prepass's), full size
#endif
layout(set = 0, binding = 30) uniform sampler2D sceneSurface;   // xy = normal (octahedral), z = roughness, w = 1: drawn
layout(set = 0, binding = 36) uniform sampler2D sceneVelocity;  // motion since last frame (uv; huge: none)

vec3 fromOctahedral(vec2 p) {
    vec3 n = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

#ifdef MS
ivec2 depthSize() { return textureSize(sceneDepth); }
#else
ivec2 depthSize() { return textureSize(sceneDepth, 0); }
#endif
// (where things are: the scene's rectangle as drawn, which a render scale makes smaller, an editor puts aside)
vec4 drawnArea() { return frame.drawnViewport; }

struct TracePoint {
    ivec2 pixel;      // the full-size pixel traced
    vec3 pos, n;      // its point in the world, its normal
    float rough, z;   // roughness, distance along the view
    bool drawn;       // (false: the sky)
};
TracePoint tracePoint() {
    TracePoint t;
    int k = int(frame.trace.z + 0.5);
    ivec2 full = depthSize();
    vec4 area = drawnArea();
    t.pixel = min(ivec2(gl_FragCoord.xy) * 2 + ivec2(k & 1, k >> 1), full - 1);
    float d = texelFetch(sceneDepth, t.pixel, 0).r;
    vec4 s = texelFetch(sceneSurface, t.pixel, 0);
    vec2 inArea = (vec2(t.pixel) + 0.5 - area.xy) / area.zw;
    t.drawn = d < 1.0 && s.w > 0.5 && all(greaterThanEqual(inArea, vec2(0.0))) && all(lessThan(inArea, vec2(1.0)));
    vec2 ndc = inArea * 2.0 - 1.0;
    vec4 w = frame.invViewProj * vec4(ndc, d, 1.0);
    t.pos = w.xyz / w.w;
    t.n = fromOctahedral(s.xy);
    t.rough = s.z;
    t.z = dot(t.pos - frame.camPos.xyz, frame.camForward.xyz);
    return t;
}
// Contact shadow: a short march towards the sun through this frame's depth; 0 where something just in front of the
// depth the ray passes behind covers it (a little thickness assumed), fading out with the distance marched
float contactShadow(TracePoint t) {
    vec3 L = normalize(-frame.sunDir.xyz);
    float reach = clamp(t.z * 0.02, 0.15, 1.0);   // (further away, a longer reach: the same few pixels)
    vec4 area = drawnArea();
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + frame.trace.z * 7.0, vec2(0.06711056, 0.00583715))));
    vec3 start = t.pos + t.n * 0.02;
    const int kSteps = 10;
    for (int i = 0; i < kSteps; ++i) {
        float s = (float(i) + noise) / float(kSteps);
        vec3 p = start + L * (reach * s);
        vec4 c = frame.viewProj * vec4(p, 1.0);
        if (c.w <= 1e-4) break;
        vec2 uv = c.xy / c.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float d = texelFetch(sceneDepth, ivec2(area.xy + uv * area.zw), 0).r;
        if (d >= 1.0) continue;
        vec4 w = frame.invViewProj * vec4(c.xy / c.w, d, 1.0);
        float sceneZ = dot(w.xyz / w.w - frame.camPos.xyz, frame.camForward.xyz);
        float rayZ = dot(p - frame.camPos.xyz, frame.camForward.xyz);
        float behind = rayZ - sceneZ;
        if (behind > 0.01 && behind < 0.15 + reach * 0.3) return mix(0.0, 1.0, s * s);   // (near: dark; far: softer)
    }
    return 1.0;
}

// Where this point was in last frame's half-size history (uv), and whether it was what's there: last frame's
// picture's distance at that place matches this point's distance from last frame's camera
bool historyAt(TracePoint t, out vec2 uv) {
    vec2 full = vec2(depthSize());
    vec4 area = drawnArea();
    vec2 v = texelFetch(sceneVelocity, t.pixel, 0).rg;   // (in the view's uv: times the drawn area's size)
    vec2 before = vec2(t.pixel) + 0.5 - (v.x < 10000.0 ? v : vec2(0.0)) * area.zw;
    uv = before / full;   // (the history: drawn as this frame is)
    vec2 inArea = (before - area.xy) / area.zw;
    if (frame.trace.y < 0.5 || any(lessThan(inArea, vec2(0.0))) || any(greaterThan(inArea, vec2(1.0)))) return false;
    // (what the last frame's picture showed there, found by its own camera: its picture's own size and place)
    vec4 c = frame.prevViewProj * vec4(t.pos, 1.0);
    if (c.w <= 1e-4) return false;
    vec2 size = vec2(textureSize(lastFrame, 0));
    vec2 at = ((c.xy / c.w * 0.5 + 0.5) * frame.prevViewport.zw + frame.prevViewport.xy) / size;
    float was = textureLod(lastFrame, at, 0.0).a;
    float expected = dot(t.pos - frame.prevCamPos.xyz, frame.prevCamForward.xyz);
    return abs(was - expected) < 0.05 * max(expected, 0.5) + 0.1;
}
