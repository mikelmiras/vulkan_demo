#version 450
// The starburst (renderer_post.cpp, after the bloom; the main view's, at a quarter of the size each way: the
// composite brings it up, binding 8): streaks through the brightest spots along the iris's blades (render/
// starburst.cpp's lines: n streaks for an even n, 2n for an odd one), each gathered back along its line from the spots
// it passes (theirs over 1 in the bloom's finest level, compressed as the flare's), fading out over a fifth of the
// view's height. Skipped where the bloom's coarse level shows nothing bright near (most of a night street).

layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(push_constant) uniform Post {
    vec4 a;   // xy = 1 / this image's size
    vec4 b, c, d, e, f;
    vec4 g;   // w = the iris's blades + the strength x 0.99 (render/starburst.cpp's pack)
} pc;
layout(location = 0) out vec4 outColor;

vec3 brightest(vec2 at, float level) {   // (composite.frag's)
    if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0) return vec3(0.0);
    vec3 over = max(textureLod(bloom, at, level).rgb - vec3(1.0), vec3(0.0));
    return over / (1.0 + dot(over, vec3(0.3333)));
}

void main() {
    vec2 uv = gl_FragCoord.xy * pc.a.xy;
    int blades = int(floor(pc.g.w));
    float strength = (pc.g.w - float(blades)) / 0.99;
    vec3 near = textureLod(bloom, uv, 5.0).rgb;
    float gate = smoothstep(0.03, 0.12, max(near.r, max(near.g, near.b)));   // (faded in: no edge where it starts)
    if (gate <= 0.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    int lines = blades % 2 == 0 ? blades / 2 : blades;
    vec2 aspect = vec2(textureSize(bloom, 0)) / float(textureSize(bloom, 0).y);
    vec3 sum = vec3(0.0);
    for (int k = 0; k < lines; ++k) {
        float a = 0.35 + 3.14159265 * float(k) / float(lines);
        vec2 dir = vec2(cos(a), sin(a)) / aspect;   // (round, not stretched by the view's shape)
        for (int j = 1; j <= 16; ++j) {
            float t = float(j) / 16.0, w = (1.0 - t) * (1.0 - t);
            vec2 step = dir * (t * 0.2);
            sum += (brightest(uv + step, 0.0) + brightest(uv - step, 0.0)) * w;   // (the finest level: thin streaks)
        }
    }
    outColor = vec4(sum * (strength * gate * 0.25 / float(lines)), 1.0);
}
