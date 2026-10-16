// Coloured shadows (renderer_shadows.cpp: see-through and thin translucent casters' tint, drawn with the shadow maps):
// what the light comes through on its way to p (glass, a curtain), if p is behind what tinted it; white elsewhere.
// The sun's in a layer per cascade (binding 38), spot and point lights' in an atlas shaped like theirs (40).
// frame.shadowParams.w: bit 1, the sun's hold something this frame; bit 2, the lamps'.
layout(set = 0, binding = 38) uniform sampler2DArray shadowTint;   // rgb let through, a = the nearest tinting depth
layout(set = 0, binding = 40) uniform sampler2D lampTintAtlas;     // (the same, in the lamps' tiles)

bool sunTinted() { return (int(frame.shadowParams.w + 0.5) & 1) != 0; }
bool lampsTinted() { return (int(frame.shadowParams.w + 0.5) & 2) != 0; }

// (the filtered colour where p is behind the nearest of the four texels' depths: an edge fades rather than steps)
vec3 tintAt(vec4 t, vec4 depths, float z) {
    float nearest = min(min(depths.x, depths.y), min(depths.z, depths.w));
    return z > nearest + 0.002 ? t.rgb : vec3(1.0);
}

vec3 sunTint(vec3 p, vec3 N) {
    float d = dot(p - frame.camPos.xyz, frame.camForward.xyz);
    int c = 0;
    while (c < 4 && d > frame.cascadeSplits[c]) ++c;
    if (c >= 4) return vec3(1.0);
    vec3 proj = (frame.shadowViewProj[c] * vec4(p + N * (0.03 + 0.03 * float(c)), 1.0)).xyz;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return vec3(1.0);
    return tintAt(textureLod(shadowTint, vec3(uv, float(c)), 0.0), textureGather(shadowTint, vec3(uv, float(c)), 3), proj.z);
}

// A spot light's (its view) or a point light's (the cube face's view) tint at p
vec3 lampTint(int view, vec3 p) {
    vec4 clip = frame.shadowViewProj[view] * vec4(p, 1.0);
    if (clip.w <= 1e-4) return vec3(1.0);
    vec3 proj = clip.xyz / clip.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0 || proj.z < 0.0) return vec3(1.0);
    vec4 rect = frame.shadowRect[view];
    vec2 at = rect.xy + uv * rect.zw;
    return tintAt(textureLod(lampTintAtlas, at, 0.0), textureGather(lampTintAtlas, at, 3), proj.z);
}
