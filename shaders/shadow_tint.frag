#version 450
// The sun's shadow tint: what a see-through or thin translucent caster lets through, multiplied into its cascade's
// tint layer (white to start), and its depth, the nearest kept (the blend: renderer.cpp's tint pipeline). Glass of
// opacity a passes its colour more the more opaque it is, and less light; a translucent one, its colour times its
// translucency (and the texture's, both).

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in vec4 vCutout;   // x = opacity, y = textured, z = alpha mode
layout(location = 2) flat in vec4 vTint;     // rgb = colour, w = translucency

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = vCutout.y > 0.5 ? texture(albedoMap, vUV) : vec4(1.0);
    vec3 base = vTint.rgb * tex.rgb;
    vec3 through;
    if (int(vCutout.z + 0.5) == 2) {   // see-through
        float alpha = clamp(vCutout.x * tex.a, 0.0, 1.0);
        through = mix(vec3(1.0), base, alpha) * (1.0 - 0.5 * alpha);
    } else {                           // thin, translucent
        if (int(vCutout.z + 0.5) == 1 && tex.a < 0.5) discard;   // (a cut-out's holes: no tint there)
        through = base * clamp(vTint.w, 0.0, 1.0);
    }
    outColor = vec4(through, gl_FragCoord.z);
}
