// The half-size traces (ssr_trace.frag, gi_trace.frag; renderer_trace.cpp): each half-size pixel stands for one of
// its 2 x 2 full-size pixels, a different one each frame, and keeps a history of the others (reprojected by the
// motion vectors, dropped where the depth says it saw something else there)
layout(set = 0, binding = 29) uniform sampler2D sceneDepth;     // this frame's (the prepass's), full size
layout(set = 0, binding = 30) uniform sampler2D sceneSurface;   // xy = normal (octahedral), z = roughness, w = 1: drawn
layout(set = 0, binding = 36) uniform sampler2D sceneVelocity;  // motion since last frame (uv; huge: none)

vec3 fromOctahedral(vec2 p) {
    vec3 n = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

struct TracePoint {
    ivec2 pixel;      // the full-size pixel traced
    vec3 pos, n;      // its point in the world, its normal
    float rough, z;   // roughness, distance along the view
    bool drawn;       // (false: the sky)
};
TracePoint tracePoint() {
    TracePoint t;
    int k = int(frame.trace.z + 0.5);
    ivec2 full = textureSize(sceneDepth, 0);
    t.pixel = min(ivec2(gl_FragCoord.xy) * 2 + ivec2(k & 1, k >> 1), full - 1);
    float d = texelFetch(sceneDepth, t.pixel, 0).r;
    vec4 s = texelFetch(sceneSurface, t.pixel, 0);
    t.drawn = d < 1.0 && s.w > 0.5;
    vec2 ndc = (vec2(t.pixel) + 0.5) / vec2(full) * 2.0 - 1.0;
    vec4 w = frame.invViewProj * vec4(ndc, d, 1.0);
    t.pos = w.xyz / w.w;
    t.n = fromOctahedral(s.xy);
    t.rough = s.z;
    t.z = dot(t.pos - frame.camPos.xyz, frame.camForward.xyz);
    return t;
}
// Where this point was in last frame's half-size history (uv), and whether it was what's there: last frame's
// picture's distance at that place matches this point's distance from last frame's camera
bool historyAt(TracePoint t, out vec2 uv) {
    ivec2 full = textureSize(sceneDepth, 0);
    vec2 here = (vec2(t.pixel) + 0.5) / vec2(full);
    vec2 v = texelFetch(sceneVelocity, t.pixel, 0).rg;
    uv = here - (v.x < 10000.0 ? v : vec2(0.0));
    if (frame.trace.y < 0.5 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    float was = textureLod(lastFrame, uv, 0.0).a;   // (the last frame's picture: full size, its own viewport)
    float expected = dot(t.pos - frame.prevCamPos.xyz, frame.prevCamForward.xyz);
    return abs(was - expected) < 0.05 * max(expected, 0.5) + 0.1;
}
