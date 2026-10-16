#version 450
// The depth prepass (renderer.cpp): opaque things' depth only (a rendering with no colour attachment), before they're
// shaded, so the lit pass after it (scene.frag with early depth tests) shades each pixel once. Holes still need
// cutting here: cut-outs' texture alpha and terrains' holes, as scene.frag cuts them.

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
#ifdef VELOCITY
layout(set = 1, binding = 2) uniform sampler2D metalRoughMap;   // (the surface's roughness for the traces)
layout(location = 1) in vec3 vNormal;
#endif

layout(location = 2) in vec2 vUV;
layout(location = 3) flat in vec4 vColor;
layout(location = 4) flat in vec4 vParams;
layout(location = 5) flat in vec4 vMaterial;
layout(location = 6) flat in vec4 vEmissive;
layout(location = 8) in float vHole;
#ifdef VELOCITY
// Without MSAA (depth_prepass_velocity.frag): each pixel's motion since last frame, in the view's uv units (temporal
// AA), and its surface: the normal (without its map, octahedral) and roughness (the half-size traces)
layout(location = 12) in vec4 vClipNow;
layout(location = 13) in vec4 vClipBefore;
layout(location = 14) flat in vec4 vScatter;   // (y: subsurface, kept in the surface's w for sss.glsl)
layout(location = 0) out vec2 outVelocity;
layout(location = 1) out vec4 outSurface;
vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 p = n.xz;
    if (n.y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p;
}
#endif

void main() {
    bool terrain = vParams.x > 1.5 && vParams.x < 2.5;
    if (terrain && vHole > 0.001) discard;
    if (!terrain && int(vMaterial.w + 0.5) == 1) {   // a cut-out
        float alpha = vColor.a * (vParams.z > 0.5 ? texture(albedoMap, vUV).a : 1.0);
        if (alpha < vEmissive.w) discard;
    }
#ifdef VELOCITY
    vec2 now = vClipNow.xy / vClipNow.w, before = vClipBefore.xy / max(vClipBefore.w, 1e-5);
    outVelocity = vClipBefore.w > 1e-5 ? (now - before) * 0.5 : vec2(0.0);
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing && vParams.x > -0.5) n = -n;   // (two-sided surfaces; foliage keeps its normals)
    float rough = vParams.x > 0.5 ? 0.9 : clamp(vMaterial.y * (vParams.x > 1.5 ? 1.0 : texture(metalRoughMap, vUV).g), 0.02, 1.0);
    float subsurface = vParams.x > 1.5 ? 0.0 : clamp(vScatter.y, 0.0, 1.0);   // (terrains and water: none)
    outSurface = vec4(octahedral(n), rough, 1.0 + subsurface);   // (w: drawn, and how much it scatters)
#endif
}
