#version 450
// Ambient occlusion before lighting, with its history and bent normal (ssao.glsl)
#extension GL_GOOGLE_include_directive : require
#define TEMPORAL
#include "ssao.glsl"
