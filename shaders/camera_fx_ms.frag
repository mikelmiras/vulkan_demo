#version 450
// Depth of field and motion blur over a multisampled depth (camera_fx.glsl)
#extension GL_GOOGLE_include_directive : require
#define MULTISAMPLED
#include "camera_fx.glsl"
