#version 450

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color; // sRGB, written as-is to the UNORM target
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(pc.color.rgb, 1.0);
}
