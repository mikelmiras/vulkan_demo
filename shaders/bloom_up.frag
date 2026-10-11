#version 450
// Bloom, coming back up: a 3x3 tent filter of the smaller level, added onto the larger one (additive blending)

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Post {
    vec4 a;   // xy = source texel size, z = filter radius (in source texels)
    vec4 b;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 d = pc.a.xy * pc.a.z;
    vec3 s = texture(source, vUV).rgb * 4.0;
    s += (texture(source, vUV + vec2(-d.x, 0)).rgb + texture(source, vUV + vec2(d.x, 0)).rgb +
          texture(source, vUV + vec2(0, -d.y)).rgb + texture(source, vUV + vec2(0, d.y)).rgb) * 2.0;
    s += texture(source, vUV + vec2(-d.x, -d.y)).rgb + texture(source, vUV + vec2(d.x, -d.y)).rgb +
         texture(source, vUV + vec2(-d.x, d.y)).rgb + texture(source, vUV + vec2(d.x, d.y)).rgb;
    outColor = vec4(s / 16.0, 1.0);
}
