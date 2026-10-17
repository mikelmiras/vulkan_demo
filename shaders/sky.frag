#version 450
// The sky behind everything (the physical sky, a sky image or the gradient), with its clouds marched through, the fog
// in front of it and the water above the camera
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "fog.glsl"
#include "clouds.glsl"

layout(set = 0, binding = 21) uniform sampler3D fogVolume;   // volumetric fog (fog.glsl): the sky is behind all of it
layout(set = 0, binding = 44) uniform sampler2D halfClouds;  // the main view's clouds at half size (clouds.frag)

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main() {
    float tanY = frame.camForward.w, aspect = frame.viewport.x / max(frame.viewport.y, 1.0);
    vec3 dir = normalize(frame.camForward.xyz + frame.camRight.xyz * (vNdc.x * tanY * aspect) -
                         frame.camUp.xyz * (vNdc.y * tanY));   // (clip-space y points down)
    // (the physical sky's clouds marched at medium and high quality; else the flat layer)
    bool volumetric = frame.atmosphere.x > 0.5 && frame.envParams.x < 0.5 && frame.atmosphere.w > 0.0 && frame.shadowParams.z > 0.5;
    skyFlatClouds = !volumetric;
    vec3 color = skyColor(dir, true);
    if (volumetric) {
        vec4 clouds;
        if (frame.skyToSun.w > 0.5) {   // (marched at half size this frame: brought up)
            vec2 area = ceil(frame.drawnViewport.zw * 0.5);
            vec2 uv = (gl_FragCoord.xy - frame.drawnViewport.xy) / frame.drawnViewport.zw;
            clouds = textureLod(halfClouds, uv * area / vec2(textureSize(halfClouds, 0)), 0.0);
        } else {
            float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + mod(frame.time.y, 64.0) * 5.588, vec2(0.06711056, 0.00583715))));
            clouds = volumetricClouds(dir, noise);
        }
        color = color * clouds.a + clouds.rgb;
    }
    if (frame.fogParams.x > 0.5) {
        vec4 f = texture(fogVolume, vec3(vNdc * 0.5 + 0.5, 1.0));
        color = color * f.a + f.rgb;
    }
    if (frame.waterInfo.z > 0.5) color = throughWater(color, frame.camPos.xyz + dir * 1e4);   // (from under the water)
    outColor = vec4(color, 1.0);
}
