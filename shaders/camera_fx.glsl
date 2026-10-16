// Camera effects over the picture after temporal AA (renderer_post.cpp), before bloom: motion blur, the picture
// smeared along each pixel's motion on screen (the prepass's motion vectors), and depth of field, a disc of taps
// as wide as the circle of confusion there (the thin lens's: |z - focus| / z), each tap counting if its own circle
// reaches this pixel; what's behind counts no more than this pixel's circle, so the background doesn't spill over a
// sharp foreground. Bright taps weigh a little more: highlights open into discs (bokeh).
layout(push_constant) uniform CameraFx {
    vec4 a;   // x = near, y = far, z = the focus distance (m), w = the largest circle of confusion (pixels; 0: no depth of field)
    vec4 b;   // the viewport: x, y, width, height (pixels)
    vec4 c;   // x = render scale (the depth and motion drawn that much smaller), y = motion blur (0..1), z = the frame
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

void main() {
    vec2 px = gl_FragCoord.xy, size = vec2(textureSize(hdr, 0));
    vec4 centre = texelFetch(hdr, ivec2(px), 0);
    if (px.x < pc.b.x || px.y < pc.b.y || px.x > pc.b.x + pc.b.z || px.y > pc.b.y + pc.b.w) {
        outColor = centre;
        return;
    }
    float noise = fract(52.9829189 * fract(dot(px + mod(pc.c.z, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
    vec3 colour = centre.rgb;
    // Motion blur: eight taps across the motion, centred on the pixel (at most a twentieth of the view)
    if (pc.c.y > 0.0) {
        vec2 v = texelFetch(velocity, drawn(px), 0).rg;
        if (v.x < 10000.0) {
            vec2 along = v * pc.b.zw * pc.c.y;
            float len = length(along), most = pc.b.w * 0.05;
            if (len > most) along *= most / len;
            if (len > 0.75) {
                vec3 sum = vec3(0.0);
                for (int i = 0; i < 8; ++i) {
                    float t = (float(i) + noise) / 8.0 - 0.5;
                    sum += textureLod(hdr, (px + along * t) / size, 0.0).rgb;
                }
                colour = sum / 8.0;
            }
        }
    }
    // Depth of field: 24 taps over a golden-angle spiral filling the circle
    if (pc.a.w > 0.0) {
        float z = distanceAt(px), circle = circleAt(z);
        if (circle > 0.75) {
            vec3 sum = colour;
            float total = 1.0;
            const int kTaps = 24;
            for (int i = 0; i < kTaps; ++i) {
                float r = sqrt((float(i) + 0.5) / float(kTaps)) * circle;
                float angle = (float(i) + noise) * 2.39996323;
                vec2 q = px + vec2(cos(angle), sin(angle)) * r;
                float tz = distanceAt(q);
                float tc = tz > z ? min(circleAt(tz), circle) : circleAt(tz);
                float w = clamp(tc - r + 1.0, 0.0, 1.0);
                vec3 tap = textureLod(hdr, q / size, 0.0).rgb;
                w *= 1.0 + clamp(dot(tap, vec3(0.3, 0.5, 0.2)) - 1.0, 0.0, 4.0);   // (highlights: discs)
                sum += tap * w;
                total += w;
            }
            colour = sum / total;
        }
    }
    outColor = vec4(colour, centre.a);   // (alpha: the outline's marker, as it was)
}
