#version 450
// Screen-space reflections at half size (renderer_trace.cpp): this frame's point of the 2 x 2 block, its mirror
// reflection traced through the last frame's picture (blurred by its roughness), blended with the history of the
// block's other points; and the point's distance, for scene.frag's upsample
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "screen_trace.glsl"
#include "trace_common.glsl"

layout(set = 0, binding = 33) uniform sampler2D history;   // last frame's

layout(location = 0) out vec4 outReflection;
layout(location = 1) out float outDepth;

void main() {
    TracePoint t = tracePoint();
    if (!t.drawn || t.rough >= 0.8 || frame.prevCamPos.w < 0.5) {   // (nothing, too rough, or GI's depth alone)
        outReflection = vec4(0.0);
        outDepth = t.drawn ? t.z : 1e4;
        return;
    }
    vec3 V = normalize(frame.camPos.xyz - t.pos);
    vec4 now = traceReflection(t.pos, reflect(-V, t.n), t.rough, vec2(t.pixel));
    vec2 uv;
    if (historyAt(t, uv)) now = mix(now, textureLod(history, uv, 0.0), 0.75);   // (four frames: the block's pixels)
    outReflection = now;
    outDepth = t.z;
}
