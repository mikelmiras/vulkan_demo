#version 450
// Screen-space reflections at half size (renderer_trace.cpp): this frame's point of the 2 x 2 block, its mirror
// reflection traced through the last frame's picture (blurred by its roughness), blended with the history of the
// block's other points; the point's distance, for scene.frag's upsample, and its contact shadow
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "screen_trace.glsl"
#include "trace_common.glsl"

layout(set = 0, binding = 33) uniform sampler2D history;   // last frame's

layout(location = 0) out vec4 outReflection;
layout(location = 1) out vec4 outDepth;   // (the distance, the sun's contact shadow, the lamp's: frame.contactLight)

void main() {
    TracePoint t = tracePoint();
    bool contacts = t.drawn && frame.trace.w > 0.5;
    float sun = contacts ? contactShadow(t, normalize(-frame.sunDir.xyz), 1.0) : 1.0, lamp = 1.0;
    if (contacts && frame.contactLight.w >= 0.0) {
        vec3 d = frame.contactLight.xyz - t.pos;
        float dist = length(d);
        if (dist > 0.05 && dot(d, t.n) > 0.0) lamp = contactShadow(t, d / dist, dist * 0.9);
    }
    outDepth = vec4(t.drawn ? t.z : 1e4, sun, lamp, 0.0);
    if (!t.drawn || t.rough >= 0.8 || frame.prevCamPos.w < 0.5) {   // (nothing, too rough, or the others' alone)
        outReflection = vec4(0.0);
        return;
    }
    vec3 V = normalize(frame.camPos.xyz - t.pos);
    vec4 now = traceReflection(t.pos, reflect(-V, t.n), t.rough, vec2(t.pixel));
    vec2 uv;
    if (historyAt(t, uv)) now = mix(now, textureLod(history, uv, 0.0), 0.75);   // (four frames: the block's pixels)
    outReflection = now;
}
