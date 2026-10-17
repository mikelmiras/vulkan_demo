// Camera effects over the picture after temporal AA (renderer_post.cpp), before bloom: motion blur, the picture
// smeared along the motion on screen (the prepass's motion vectors), and depth of field, a disc of taps as wide as
// the circle of confusion there (the thin lens's: |z - focus| / z), each tap counting if its own circle reaches this
// pixel. Bright taps weigh a little more: highlights open into discs (bokeh).
// Both reach past a pixel's own motion or circle: TILES (a pass at a texel per 8 x 8 pixels) finds each tile's
// largest near circle and longest motion, DILATE (another, as small) the largest of the 3 x 3 tiles round each, which
// the main pass reads once. Motion blur gathers along the neighbourhood's longest motion and weighs each tap by
// whether its own motion, or this pixel's, spans the gap, by which is in front (McGuire et al.'s reconstruction
// filter): a moving thing smears over the still background beside it, not only within itself. Depth of field keeps
// the near field (taps in front of this pixel and blurrier) apart and lays it over the rest by how much of the disc
// it covers, so a blurred foreground's edge fades out evenly rather than showing the background through; a blurred
// foreground pixel takes what's behind it within its own circle (its edge half see-through, as through a lens), a
// sharper one no more than its circle (the background doesn't spill over a sharp foreground). The sky moves with the
// camera's turn (the motion vectors have nothing there: skyMotion's matrix, render/camera_fx.h).
layout(push_constant) uniform CameraFx {
    vec4 a;   // x = near, y = far, z = the focus distance (m), w = the largest circle of confusion (pixels; 0: no depth of field)
    vec4 b;   // the viewport: x, y, width, height (pixels)
    vec4 c;   // x = render scale (the depth and motion drawn that much smaller), y = motion blur (0..1), z = the frame
    vec4 d;   // xyz = the sky's motion matrix's first row, w = 1: the sky moves (the camera turned)
    vec4 e;   // its second row
    vec4 f;   // its third
} pc;
layout(set = 0, binding = 0) uniform sampler2D hdr;
#ifdef MULTISAMPLED
layout(set = 0, binding = 1) uniform sampler2DMS depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
ivec2 depthSize() { return textureSize(depthMap); }
#else
layout(set = 0, binding = 1) uniform sampler2D depthMap;
float depthAt(ivec2 p) { return texelFetch(depthMap, p, 0).r; }
ivec2 depthSize() { return textureSize(depthMap, 0); }
#endif
layout(set = 0, binding = 2) uniform sampler2D velocity;   // (the view's uv per frame; sky and unwritten: >= 10000)
layout(set = 0, binding = 3) uniform sampler2D tiles;       // (DILATE's) each 8 x 8 tile's: r = the largest near
                                                             // circle, gb = the longest motion (pixels)
layout(set = 0, binding = 4) uniform sampler2D dilated;     // (the main pass's) the same, the largest round each tile

layout(location = 0) out vec4 outColor;

// (a pixel of the picture: where it is in the depth and motion drawn, a render scale's smaller)
ivec2 drawn(vec2 px) {
    vec2 p = pc.c.x > 0.0 && pc.c.x < 0.999 ? pc.b.xy + (px - pc.b.xy) * pc.c.x : px;
    return clamp(ivec2(p), ivec2(0), depthSize() - 1);
}
float distanceAt(vec2 px) {
    float d = depthAt(drawn(px));
    return d >= 0.99999 ? pc.a.y : pc.a.x * pc.a.y / (pc.a.y - d * (pc.a.y - pc.a.x));
}
float circleAt(float z) { return pc.a.w * clamp(abs(z - pc.a.z) / max(z, 1e-3), 0.0, 1.0); }

// The blur's motion at a pixel (pixels, as far as the shutter's share of the frame, at most a twentieth of the view):
// the motion vectors', or the sky's with the camera's turn; none without motion blur
vec2 motionAt(vec2 px) {
    if (pc.c.y <= 0.0) return vec2(0.0);
    vec2 v = texelFetch(velocity, drawn(px), 0).rg;
    if (v.x >= 10000.0 && pc.d.w > 0.5) {   // the sky: where its direction was on screen last frame
        vec3 ndc = vec3((px - pc.b.xy) / pc.b.zw * 2.0 - 1.0, 1.0);
        vec3 h = vec3(dot(pc.d.xyz, ndc), dot(pc.e.xyz, ndc), dot(pc.f.xyz, ndc));
        v = h.z > 1e-4 ? (ndc.xy - h.xy / h.z) * 0.5 : vec2(0.0);
    }
    if (v.x >= 10000.0) return vec2(0.0);
    vec2 along = v * pc.b.zw * pc.c.y;
    float len = length(along), most = pc.b.w * 0.05;
    return len > most ? along * (most / len) : along;
}

#if defined(TILES)
// A tile's largest circle of confusion nearer than the focus and its longest motion (4 x 4 of its 8 x 8 pixels)
void main() {
    vec2 corner = floor(gl_FragCoord.xy) * 8.0;
    float most = 0.0;
    vec2 fastest = vec2(0.0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            vec2 px = corner + vec2(x, y) * 2.0 + 1.0;
            if (px.x < pc.b.x || px.y < pc.b.y || px.x > pc.b.x + pc.b.z || px.y > pc.b.y + pc.b.w) continue;
            if (pc.a.w > 0.0) {
                float z = distanceAt(px);
                if (z < pc.a.z) most = max(most, circleAt(z));
            }
            vec2 m = motionAt(px);
            if (dot(m, m) > dot(fastest, fastest)) fastest = m;
        }
    outColor = vec4(most, fastest, 0.0);
}
#elif defined(DILATE)
// The largest near circle and the longest motion of the 3 x 3 tiles round this one
void main() {
    ivec2 tile = ivec2(gl_FragCoord.xy), count = textureSize(tiles, 0);
    vec4 most = vec4(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 t = texelFetch(tiles, clamp(tile + ivec2(x, y), ivec2(0), count - 1), 0);
            most.r = max(most.r, t.r);
            if (dot(t.gb, t.gb) > dot(most.gb, most.gb)) most.gb = t.gb;
        }
    outColor = most;
}
#else
void main() {
    vec2 px = gl_FragCoord.xy, size = vec2(textureSize(hdr, 0));
    vec4 centre = texelFetch(hdr, ivec2(px), 0);
    if (px.x < pc.b.x || px.y < pc.b.y || px.x > pc.b.x + pc.b.z || px.y > pc.b.y + pc.b.w) {
        outColor = centre;
        return;
    }
    float noise = fract(52.9829189 * fract(dot(px + mod(pc.c.z, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
    vec3 colour = centre.rgb;
    vec4 tile4 = texelFetch(dilated, clamp(ivec2(px) / 8, ivec2(0), textureSize(dilated, 0) - 1), 0);
    // Motion blur: eight taps along the neighbourhood's longest motion, centred on the pixel, each counting if its
    // own motion reaches here and it's in front, or this pixel's reaches it and it's behind, or both reach (McGuire)
    if (pc.c.y > 0.0 && dot(tile4.gb, tile4.gb) > 0.5625) {
        vec2 most = tile4.gb;
        float z = distanceAt(px), here = max(length(motionAt(px)), 0.5);
        float wsum = 1.0 / here;
        vec3 sum = colour * wsum;
        for (int i = 0; i < 8; ++i) {
            float t = (float(i) + noise) / 8.0 - 0.5;
            vec2 q = px + most * t;
            float d = length(most * t), there = max(length(motionAt(q)), 0.5), tz = distanceAt(q);
            float front = clamp((z - tz) / (0.02 * z + 0.05) + 0.5, 0.0, 1.0);   // (the tap in front: 1)
            float w = front * clamp(1.0 - d / there, 0.0, 1.0) + (1.0 - front) * clamp(1.0 - d / here, 0.0, 1.0) +
                      2.0 * (1.0 - smoothstep(0.95 * there, 1.05 * there, d)) * (1.0 - smoothstep(0.95 * here, 1.05 * here, d));
            sum += textureLod(hdr, q / size, 0.0).rgb * w;
            wsum += w;
        }
        colour = sum / wsum;
    }
    // Depth of field: 24 taps over a golden-angle spiral filling the circle (or the near field's round it): the near
    // field (in front and blurrier) apart, laid over the rest by the share of the disc it covers
    if (pc.a.w > 0.0) {
        float z = distanceAt(px), circle = circleAt(z);
        float reach = max(circle, tile4.r);
        if (reach > 0.75) {
            bool blurredNear = z < pc.a.z;   // (a blurred foreground: what's behind it shows at its edge)
            vec3 sum = colour, nearSum = vec3(0.0);
            float total = 1.0, nearTotal = 0.0, covered = 0.0;
            const int kTaps = 24;
            for (int i = 0; i < kTaps; ++i) {
                float r = sqrt((float(i) + 0.5) / float(kTaps)) * reach;
                float angle = (float(i) + noise) * 2.39996323;
                vec2 q = px + vec2(cos(angle), sin(angle)) * r;
                float tz = distanceAt(q), own = circleAt(tz);
                vec3 tap = textureLod(hdr, q / size, 0.0).rgb;
                float bright = 1.0 + clamp(dot(tap, vec3(0.3, 0.5, 0.2)) - 1.0, 0.0, 4.0);   // (highlights: discs)
                if (tz < z * 0.98 && own > circle + 0.5) {   // the near field over this pixel
                    float w = clamp(own - r + 1.0, 0.0, 1.0);
                    covered += w;
                    nearSum += tap * w * bright;
                    nearTotal += w * bright;
                } else {
                    float tc = tz > z ? (blurredNear ? circle : min(own, circle)) : own;
                    float w = clamp(tc - r + 1.0, 0.0, 1.0) * bright;
                    sum += tap * w;
                    total += w;
                }
            }
            colour = sum / total;
            if (nearTotal > 0.0) colour = mix(colour, nearSum / nearTotal, clamp(covered / float(kTaps), 0.0, 1.0));
        }
    }
    outColor = vec4(colour, centre.a);   // (alpha: the outline's marker, as it was)
}
#endif
