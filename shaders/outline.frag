#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 3) flat in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor.rgb, 1.0);   // sRGB, written as-is to the UNORM target
}
