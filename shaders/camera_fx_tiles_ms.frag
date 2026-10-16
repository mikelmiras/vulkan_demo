#version 450
// Depth of field's near field over a multisampled depth (camera_fx.glsl)
#extension GL_GOOGLE_include_directive : require
#define TILES
#define MULTISAMPLED
#include "camera_fx.glsl"
