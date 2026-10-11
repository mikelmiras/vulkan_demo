#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 3) flat in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    // sRGB, marked with alpha 2: the post pass shows it as is (no tonemapping, no bloom)
    outColor = vec4(vColor.rgb, 2.0);
}
