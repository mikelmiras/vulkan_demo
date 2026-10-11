#version 450
// Shadow casters with holes: cut-outs cast no shadow where their texture is open (below their cutoff), see-through
// ones only where they're mostly opaque (glass: none)

layout(set = 1, binding = 0) uniform sampler2D albedoMap;

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in vec4 vCutout;

void main() {
    float alpha = vCutout.x * (vCutout.y > 0.5 ? texture(albedoMap, vUV).a : 1.0);
    float cutoff = int(vCutout.z + 0.5) == 1 ? vCutout.w : 0.5;
    if (alpha < cutoff) discard;
}
