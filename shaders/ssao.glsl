// Screen-space ambient occlusion from the scene's depth: how much of the hemisphere above each point nearby
// geometry hides, as a factor for the composite (1: open). Half size; a 12-sample kernel turned per pixel.
// TEMPORAL (AO before lighting: renderer_trace.cpp): the kernel also turns every frame and the result is blended
// with last frame's (where this point was, by the motion vectors; dropped where the distance there differs), and
// it's an RGBA16F: r = the occlusion, g = the distance (for next frame's check), ba = the bent normal (the open
// sky's mean direction, world space, octahedral) for the indirect light.
layout(push_constant) uniform Ao {
    vec4 a;   // tan(fovX / 2), tan(fovY / 2), near, far
    vec4 b;   // radius (m), strength, viewport x, y (pixels)
    vec4 c;   // viewport width, height, render scale (the depth drawn that much smaller, in the viewport's corner),
              // w = samples (12 at high quality, 8 at medium; 0: 12)
    vec4 d;   // (TEMPORAL) x = the frame (0..63), y = 1: last frame's is good, z = 1: no motion vectors (the camera
              // held still: last frame's where this one's is)
    vec4 e;   // (TEMPORAL) the camera's right (world), w unused
    vec4 f;   // (TEMPORAL) the camera's up (world)
} pc;
// (pixels are the full-size picture's: the depth is read where each is in the picture drawn)
ivec2 drawn(ivec2 p) { return pc.c.z > 0.0 && pc.c.z < 0.999 ? ivec2(pc.b.zw + (vec2(p) - pc.b.zw) * pc.c.z) : p; }
#ifdef MULTISAMPLED
layout(set = 0, binding = 0) uniform sampler2DMS depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, drawn(p), 0).r; }
ivec2 depthSize() { return textureSize(depthMap); }
#else
layout(set = 0, binding = 0) uniform sampler2D depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, drawn(p), 0).r; }
ivec2 depthSize() { return textureSize(depthMap, 0); }
#endif

#ifdef TEMPORAL
layout(set = 0, binding = 1) uniform sampler2D velocityMap;   // each pixel's motion (the view's uv; none: >= 10000)
layout(set = 0, binding = 2) uniform sampler2D history;       // last frame's result (this pass's)
vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 p = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}
vec3 fromOct(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
#endif

layout(location = 0) out vec4 outColor;

float linearDepth(float d) { return pc.a.z * pc.a.w / (pc.a.w - d * (pc.a.w - pc.a.z)); }

// The view-space point at a (full-size) pixel
vec3 viewPos(vec2 pixel, float z) {
    vec2 ndc = (pixel - pc.b.zw) / pc.c.xy * 2.0 - 1.0;
    return vec3(ndc.x * pc.a.x * z, -ndc.y * pc.a.y * z, -z);
}
vec2 toPixel(vec3 v) {
    float z = -v.z;
    vec2 ndc = vec2(v.x / (z * pc.a.x), -v.y / (z * pc.a.y));
    return pc.b.zw + (ndc * 0.5 + 0.5) * pc.c.xy;
}

void main() {
    ivec2 size = depthSize();
    ivec2 p = clamp(ivec2(gl_FragCoord.xy * 2.0), ivec2(0), size - 1);
    vec2 px = vec2(p) + 0.5;
#ifdef TEMPORAL
    const vec4 open = vec4(1.0, 0.0, 0.5, 0.5);   // (g = 0: no distance, no bent normal)
#else
    const vec4 open = vec4(1.0);
#endif
    if (px.x < pc.b.z || px.y < pc.b.w || px.x > pc.b.z + pc.c.x || px.y > pc.b.w + pc.c.y) { outColor = open; return; }
    float d = depthAt(p);
    if (d >= 0.99999) { outColor = open; return; }   // the sky
    float z = linearDepth(d);
    vec3 P = viewPos(px, z);
    // The normal from the neighbours on the nearer side (not across an edge)
    vec3 r = viewPos(px + vec2(2, 0), linearDepth(depthAt(min(p + ivec2(2, 0), size - 1)))) - P;
    vec3 l = P - viewPos(px - vec2(2, 0), linearDepth(depthAt(max(p - ivec2(2, 0), ivec2(0)))));
    vec3 u = viewPos(px + vec2(0, 2), linearDepth(depthAt(min(p + ivec2(0, 2), size - 1)))) - P;
    vec3 dn = P - viewPos(px - vec2(0, 2), linearDepth(depthAt(max(p - ivec2(0, 2), ivec2(0)))));
    // (across the screen x down it: towards the viewer for any surface seen from the front)
    vec3 N = normalize(cross(abs(u.z) < abs(dn.z) ? u : dn, abs(r.z) < abs(l.z) ? r : l));
    if (dot(N, -P) < 0.0) N = -N;
    vec3 T = normalize(abs(N.y) < 0.9 ? cross(N, vec3(0, 1, 0)) : cross(N, vec3(1, 0, 0)));
    vec3 B = cross(N, T);
    float spin = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
#ifdef TEMPORAL
    spin += pc.d.x * 2.3999632;   // (a different turn each frame: the history gathers them)
    vec3 bent = vec3(0.0);
#endif
    float radius = pc.b.x, occluded = 0.0;
    int kSamples = pc.c.w > 0.5 ? int(pc.c.w + 0.5) : 12;
    for (int i = 0; i < kSamples; ++i) {
        float f = (float(i) + 0.5) / float(kSamples);
        float ang = float(i) * 2.399963 + spin;               // golden-angle spiral, turned per pixel
        float h = sqrt(1.0 - f);                               // cosine-weighted height in the hemisphere
        vec3 dir = T * (cos(ang) * sqrt(f)) + B * (sin(ang) * sqrt(f)) + N * h;
        vec3 S = P + dir * radius * mix(0.2, 1.0, f * f);
        if (S.z > -pc.a.z) continue;
        ivec2 sp = ivec2(toPixel(S));
        if (sp.x < 0 || sp.y < 0 || sp.x >= size.x || sp.y >= size.y) continue;
        float sceneZ = linearDepth(depthAt(sp));
        float range = smoothstep(0.0, 1.0, radius / max(abs(z - sceneZ), 1e-4));
        float hidden = (sceneZ < -S.z - 0.02 ? 1.0 : 0.0) * range;
        occluded += hidden;
#ifdef TEMPORAL
        bent += dir * (1.0 - hidden);
#endif
    }
    float ao = clamp(1.0 - pc.b.y * occluded / float(kSamples) * 1.6, 0.0, 1.0);
#ifndef TEMPORAL
    outColor = vec4(ao);
}
#else
    // (samples off screen or in front of the near plane: their directions count as open)
    vec3 bentView = dot(bent, bent) > 1e-6 ? normalize(bent) : N;
    vec3 back = cross(pc.e.xyz, pc.f.xyz);   // (view space's +z, towards the viewer)
    vec3 bentWorld = normalize(pc.e.xyz * bentView.x + pc.f.xyz * bentView.y + back * bentView.z);
    // Last frame's, where this point was: blended in if it was this point (its distance close to this one's)
    if (pc.d.y > 0.5) {
        vec2 v = pc.d.z > 0.5 ? vec2(0.0) : texelFetch(velocityMap, p, 0).rg;
        if (v.x < 10000.0) {
            vec2 before = (vec2(p) + 0.5 - v * pc.c.xy) / vec2(size);
            if (all(greaterThanEqual(before, vec2(0.0))) && all(lessThan(before, vec2(1.0)))) {
                vec4 h = textureLod(history, before, 0.0);
                if (h.g > 0.0 && abs(h.g - z) < 0.05 * z + 0.05) {
                    ao = mix(h.r, ao, 0.2);
                    bentWorld = normalize(mix(fromOct(h.ba), bentWorld, 0.2));
                }
            }
        }
    }
    outColor = vec4(ao, z, octahedral(bentWorld));
}
#endif
