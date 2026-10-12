// Screen-space reflections, the capture (renderer_post.cpp): this frame's picture (rgb) and how far each pixel is
// along the view (a, metres; far for the sky), read by scene.frag next frame to find what a reflected ray meets.
#ifdef MULTISAMPLED
layout(set = 0, binding = 0) uniform sampler2DMS depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
#else
layout(set = 0, binding = 0) uniform sampler2D depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
#endif
layout(set = 0, binding = 1) uniform sampler2D hdr;
layout(push_constant) uniform Capture {
    vec4 a;   // near, far
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = depthAt(p);
    float z = d >= 1.0 ? 1e6 : pc.a.x * pc.a.y / (pc.a.y - d * (pc.a.y - pc.a.x));
    outColor = vec4(texelFetch(hdr, p, 0).rgb, z);
}
