#version 450
// Depth of field's and motion blur's tiles, each the largest of the 3 x 3 round it (camera_fx.glsl)
#extension GL_GOOGLE_include_directive : require
#define DILATE
#include "camera_fx.glsl"
