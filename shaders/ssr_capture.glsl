// Screen-space reflections, the capture (renderer_post.cpp): this frame's picture (rgb) and how far each pixel is
// along the view (a, metres; far for the sky), read by scene.frag next frame to find what a reflected ray meets.
layout(push_constant) uniform Capture {
    vec4 a;   // near, far
    vec4 b;   // the viewport's corner (pixels), the render scale (the depth drawn that much smaller there)
} pc;
ivec2 drawn(ivec2 p) { return pc.b.z > 0.0 && pc.b.z < 0.999 ? ivec2(pc.b.xy + (vec2(p) - pc.b.xy) * pc.b.z) : p; }
#ifdef MULTISAMPLED
layout(set = 0, binding = 0) uniform sampler2DMS depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, drawn(p), 0).r; }
#else
layout(set = 0, binding = 0) uniform sampler2D depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, drawn(p), 0).r; }
#endif
layout(set = 0, binding = 1) uniform sampler2D hdr;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = depthAt(p);
    float z = d >= 1.0 ? 1e6 : pc.a.x * pc.a.y / (pc.a.y - d * (pc.a.y - pc.a.x));
    vec3 c = texelFetch(hdr, p, 0).rgb;
    // (nothing that isn't a number goes into next frame's reflections: one would feed itself frame after frame)
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    outColor = vec4(min(c, vec3(256.0)), z);
}
