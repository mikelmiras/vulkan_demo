// Screen-space subsurface diffusion (renderer.cpp, after the lit pass, before temporal AA): light spread under
// subsurface materials (skin, wax, marble: the prepass's surface w = 1 + their subsurface), a separable blur of their
// diffuse light across, then down, as wide as the scatter there (up to 2 cm in the world at full subsurface), each
// channel as far as the material's profile says (its subsurface colour: skin's red the widest), counting only taps on
// subsurface pixels at about the same depth (the background doesn't bleed in, nor they into it). Their specular light,
// which the lit pass kept apart (binding 3), is taken off each tap and put back sharp. Everything else passes through.
layout(push_constant) uniform Sss {
    vec4 a;   // xy = the direction (1, 0 or 0, 1), z = the scatter (m at full subsurface), w = tan(fovY / 2)
    vec4 b;   // the viewport: x, y, width, height (pixels)
    vec4 c;   // x = near, y = far, z = render scale (the depth and surface drawn that much smaller)
} pc;
layout(set = 0, binding = 0) uniform sampler2D picture;
#ifdef MULTISAMPLED
layout(set = 0, binding = 1) uniform sampler2DMS depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
ivec2 depthSize() { return textureSize(depthMap); }
#else
layout(set = 0, binding = 1) uniform sampler2D depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
ivec2 depthSize() { return textureSize(depthMap, 0); }
#endif
layout(set = 0, binding = 2) uniform sampler2D surface;   // (the prepass's: w = 1 + subsurface where drawn)
layout(set = 0, binding = 3) uniform usampler2D kept;     // (the specular: rgb half floats, a = the profile, 5:6:5)

vec3 specularAt(ivec2 p) {
    uvec4 k = texelFetch(kept, p, 0);
    return vec3(unpackHalf2x16(k.r).x, unpackHalf2x16(k.g).x, unpackHalf2x16(k.b).x);
}

layout(location = 0) out vec4 outColor;

ivec2 drawn(vec2 px) {
    vec2 p = pc.c.z > 0.0 && pc.c.z < 0.999 ? pc.b.xy + (px - pc.b.xy) * pc.c.z : px;
    return clamp(ivec2(p), ivec2(0), depthSize() - 1);
}
float distanceAt(ivec2 p) {
    float d = depthAt(p);
    return pc.c.x * pc.c.y / (pc.c.y - d * (pc.c.y - pc.c.x));
}

void main() {
    vec2 px = gl_FragCoord.xy;
    vec4 centre = texelFetch(picture, ivec2(px), 0);
    outColor = centre;
    if (px.x < pc.b.x || px.y < pc.b.y || px.x > pc.b.x + pc.b.z || px.y > pc.b.y + pc.b.w) return;
    ivec2 at = drawn(px);
    float s = texelFetch(surface, at, 0).w - 1.0;
    if (s <= 0.01 || centre.a > 1.0) return;   // (not subsurface; the outline's marker kept)
    float z = distanceAt(at);
    float radius = s * pc.a.z * pc.b.w / (2.0 * z * pc.a.w);   // (pixels: the scatter seen from here)
    if (radius < 0.5) return;
    uint packed = texelFetch(kept, ivec2(px), 0).a;
    vec3 widths = max(vec3(packed >> 11, (packed >> 5) & 63u, packed & 31u) / vec3(31.0, 63.0, 31.0), vec3(0.05));
    vec3 shine = specularAt(ivec2(px));
    vec3 sum = vec3(0.0), total = vec3(0.0);
    for (int i = -5; i <= 5; ++i) {
        float r = float(i) / 5.0;
        vec2 q = px + pc.a.xy * (r * radius);
        ivec2 qa = drawn(q);
        float ts = texelFetch(surface, qa, 0).w - 1.0;
        if (ts <= 0.01 && i != 0) continue;
        float tz = distanceAt(qa);
        float near = clamp(1.0 - abs(tz - z) / (s * pc.a.z * 2.0 + 0.01), 0.0, 1.0);   // (the same surface)
        vec3 w = exp(-(r * r) / (2.0 * widths * widths * 0.25)) * (i == 0 ? 1.0 : near);
        ivec2 qp = clamp(ivec2(q), ivec2(0), textureSize(picture, 0) - 1);
        vec3 diffuse = i == 0 ? centre.rgb - shine : texelFetch(picture, qp, 0).rgb - specularAt(qp);
        sum += max(diffuse, vec3(0.0)) * w;
        total += w;
    }
    outColor = vec4(sum / max(total, vec3(1e-4)) + shine, centre.a);
}
