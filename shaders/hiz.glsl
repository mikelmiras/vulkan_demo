// The depth pyramid for occlusion culling (renderer_cull.cpp): each level keeps the farthest depth of the 2x2 (or
// 3, at an odd edge) texels under it, so a box nearer than a level's texels is hidden behind what drew there.
// hiz_depth(_ms).comp makes level 0 from the depth buffer's viewport (multisampled: the farthest sample);
// hiz_reduce.comp each level from the one before.

layout(local_size_x = 8, local_size_y = 8) in;
#ifdef MULTISAMPLED
layout(set = 0, binding = 0) uniform sampler2DMS src;
#else
layout(set = 0, binding = 0) uniform sampler2D src;
#endif
layout(set = 0, binding = 1, r32f) uniform writeonly image2D dst;
layout(push_constant) uniform Push {
    ivec4 a;   // xy: where the viewport starts in the depth buffer; zw: this level's size
    ivec4 b;   // x: samples (multisampled depth)
} pc;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= pc.a.z || p.y >= pc.a.w) return;
    float d = 0.0;
#ifdef REDUCE
    ivec2 size = textureSize(src, 0), q = p * 2;
    ivec2 last = ivec2(p.x == pc.a.z - 1 ? size.x - 1 : q.x + 1, p.y == pc.a.w - 1 ? size.y - 1 : q.y + 1);
    for (int y = q.y; y <= min(last.y, size.y - 1); ++y)
        for (int x = q.x; x <= min(last.x, size.x - 1); ++x) d = max(d, texelFetch(src, ivec2(x, y), 0).r);
#elif defined(MULTISAMPLED)
    for (int s = 0; s < pc.b.x; ++s) d = max(d, texelFetch(src, p + pc.a.xy, s).r);
#else
    d = texelFetch(src, p + pc.a.xy, 0).r;
#endif
    imageStore(dst, p, vec4(d));
}
