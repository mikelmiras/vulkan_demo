#version 450
// Ambient occlusion before lighting over a multisampled depth, with its history and bent normal (ssao.glsl)
#extension GL_GOOGLE_include_directive : require
#define TEMPORAL
#define MULTISAMPLED
#include "ssao.glsl"
