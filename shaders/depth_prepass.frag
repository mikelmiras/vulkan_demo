#version 450
// The depth prepass (renderer.cpp): opaque things' depth only (a rendering with no colour attachment), before they're
// shaded, so the lit pass after it (scene.frag with early depth tests) shades each pixel once. Holes still need
// cutting here: cut-outs' texture alpha and terrains' holes, as scene.frag cuts them.

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 2) in vec2 vUV;
layout(location = 3) flat in vec4 vColor;
layout(location = 4) flat in vec4 vParams;
layout(location = 5) flat in vec4 vMaterial;
layout(location = 6) flat in vec4 vEmissive;
layout(location = 8) in float vHole;

void main() {
    bool terrain = vParams.x > 1.5 && vParams.x < 2.5;
    if (terrain && vHole > 0.001) discard;
    if (!terrain && int(vMaterial.w + 0.5) == 1) {   // a cut-out
        float alpha = vColor.a * (vParams.z > 0.5 ? texture(albedoMap, vUV).a : 1.0);
        if (alpha < vEmissive.w) discard;
    }
}
