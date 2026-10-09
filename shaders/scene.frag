#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 camPos;
    vec4 lightDir;
    vec4 fogColor;
    vec4 viewport;
} frame;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 base = pc.color.rgb;
    bool ground = pc.color.a < 0.5;

    if (ground) {
        // 2m checkerboard plus anti-aliased 10m grid lines
        vec2 cell = floor(vWorldPos.xz / 2.0);
        float checker = mod(cell.x + cell.y, 2.0);
        base = mix(base, base * 0.7, checker);
        vec2 gp = vWorldPos.xz / 10.0;
        vec2 g = abs(fract(gp - 0.5) - 0.5) / fwidth(gp);
        float line = 1.0 - min(min(g.x, g.y), 1.0);
        base = mix(base, vec3(0.8), line * 0.6);
    }

    vec3 N = normalize(vNormal);
    vec3 L = normalize(-frame.lightDir.xyz);
    vec3 V = normalize(frame.camPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0);
    float spec = diff > 0.0 ? pow(max(dot(N, H), 0.0), 48.0) * (ground ? 0.05 : 0.35) : 0.0;

    vec3 skyAmb = vec3(0.45, 0.55, 0.75);
    vec3 groundAmb = vec3(0.20, 0.18, 0.15);
    vec3 ambient = mix(groundAmb, skyAmb, N.y * 0.5 + 0.5) * 0.45;

    vec3 color = base * (ambient + diff * vec3(1.0, 0.95, 0.85)) + vec3(spec);

    float dist = length(frame.camPos.xyz - vWorldPos);
    float fog = 1.0 - exp(-pow(dist * frame.fogColor.w, 2.0));
    color = mix(color, frame.fogColor.rgb, fog);

    // Swapchain is UNORM (so the UI's sRGB colours stay correct): gamma-encode here
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
