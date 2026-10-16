#version 450
// Screen-space global illumination at half size (renderer_trace.cpp): this frame's point of the 2 x 2 block, its
// rays traced through the last frame's picture (more of them than scene.frag can afford per pixel), blended with the
// history of the block's other points
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "screen_trace.glsl"
#include "trace_common.glsl"

layout(set = 0, binding = 34) uniform sampler2D history;   // last frame's

layout(location = 0) out vec4 outLight;

void main() {
    TracePoint t = tracePoint();
    if (!t.drawn) {
        outLight = vec4(0.0);
        return;
    }
    bool fine = frame.shadowParams.z > 1.5;
    vec4 now = traceIrradiance(t.pos, t.n, vec2(t.pixel), fine ? 4 : 2, fine ? 10 : 7, mod(frame.time.y, 64.0) * 5.588);
    vec2 uv;
    if (historyAt(t, uv)) now = mix(now, textureLod(history, uv, 0.0), 0.85);
    outLight = now;
}
