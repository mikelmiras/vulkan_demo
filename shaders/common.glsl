// Shared by the scene shaders: per-frame data (set 0, binding 0) and per-object instance data (set 0, binding 2).
// Must match FrameUBO / InstanceData in src/render/renderer.cpp.

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 shadowViewProj[52];  // 0-3: the sun's cascades, then spot and point lights' views in the shadow atlas
    vec4 shadowRect[52];      // atlas views: the tile (uv offset, uv size)
    vec4 camPos;
    vec4 fogColor;        // rgb = fog/sky colour, w = density
    vec4 viewport;        // width, height, outline width (px), unused
    vec4 sunDir;          // xyz = direction the light travels, w = 1 when it casts shadows
    vec4 sunColor;        // rgb = colour * intensity (linear)
    vec4 ambient;         // rgb = ambient strength, w = number of point and spot lights (the lights buffer)
    vec4 clusterGrid;     // tiles x, y, depth slices; w = 1: clustered (0: every light)
    vec4 clusterDepth;    // near, slices / log(far / near)
    vec4 camRight;        // the camera's axes in the world: billboards face it
    vec4 camUp;
    vec4 camForward;      // w = tan(fovY / 2)
    vec4 cascadeSplits;   // far end of each sun shadow cascade, metres along the view
    vec4 zenith;          // sky overhead (linear)
    vec4 ground;          // below the horizon (linear)
    vec4 envParams;       // a sky image: x = 1 if there is one, y = brightness, z = turn around the vertical
    vec4 sh[9];           // its irradiance as spherical harmonics (rgb)
    vec4 probePos[4];     // reflection probes: xyz = where it was baked, w = 1 if used
    vec4 probeMin[4];     // its box
    vec4 probeMax[4];     // w = 1: box projection
    vec4 probeSh[4 * 9];  // its irradiance (nine per probe)
} frame;

// The sky image (render/environment.h): an equirectangular panorama, and its radiance prefiltered by roughness
layout(set = 0, binding = 7) uniform sampler2D skyImage;
layout(set = 0, binding = 8) uniform sampler2D skyRadiance;
const float kPi = 3.14159265;
const float kRadianceLevels = 6.0;

vec3 skyImageDir(vec3 d) {   // (turned with the sky)
    float c = cos(frame.envParams.z), s = sin(frame.envParams.z);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}
vec2 equirect(vec3 d) { return vec2(atan(d.x, -d.z) / (2.0 * kPi) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / kPi); }

// Light a matte surface facing `n` gets from the sky image (divided by pi: times the albedo, it's what it reflects)
vec3 skyIrradiance(vec3 n) {
    vec3 d = skyImageDir(normalize(n));
    vec3 e = frame.sh[0].rgb * 0.282095 + frame.sh[1].rgb * (0.488603 * d.y) + frame.sh[2].rgb * (0.488603 * d.z) +
             frame.sh[3].rgb * (0.488603 * d.x) + frame.sh[4].rgb * (1.092548 * d.x * d.y) +
             frame.sh[5].rgb * (1.092548 * d.y * d.z) + frame.sh[6].rgb * (0.315392 * (3.0 * d.z * d.z - 1.0)) +
             frame.sh[7].rgb * (1.092548 * d.x * d.z) + frame.sh[8].rgb * (0.546274 * (d.x * d.x - d.y * d.y));
    return max(e, vec3(0.0)) / kPi * frame.envParams.y;
}
// The sky image reflected along `r` by a surface this rough
vec3 skyReflection(vec3 r, float rough) {
    return textureLod(skyRadiance, equirect(skyImageDir(r)), rough * (kRadianceLevels - 1.0)).rgb * frame.envParams.y;
}

// The sky in a direction: the sky image if there is one; else horizon (the fog colour) to zenith above, to ground
// below, with the sun's disc and glow if asked
vec3 skyColor(vec3 dir, bool withSun) {
    if (frame.envParams.x > 0.5) return textureLod(skyImage, equirect(skyImageDir(normalize(dir))), 0.0).rgb * frame.envParams.y;
    vec3 horizon = frame.fogColor.rgb;
    vec3 c = dir.y >= 0.0 ? mix(horizon, frame.zenith.rgb, pow(clamp(dir.y, 0.0, 1.0), 0.6))
                          : mix(horizon, frame.ground.rgb, pow(clamp(-dir.y * 4.0, 0.0, 1.0), 0.5));
    if (withSun) {
        float s = max(dot(dir, -frame.sunDir.xyz), 0.0);
        c += frame.sunColor.rgb * (smoothstep(0.9995, 0.9998, s) * 8.0 + pow(s, 12.0) * 0.12);
    }
    return c;
}

// One per drawn object; draws are instanced, gl_InstanceIndex picks the object
struct Instance {
    mat4 model;
    vec4 color;    // rgb = albedo (linear; sRGB for the outline), a unused
    vec4 params;   // x = 1 for the procedural checker ground, y = texture tiling, z = 1 if textured,
                   // w = first joint matrix (skinned meshes) or -1
    vec4 material; // x = metallic, y = roughness, z = normal map strength (0: no normal map), w = alpha mode
                   // (0 opaque, 1 cut-out, 2 see-through); color.a = opacity
    vec4 emissive; // rgb = emitted light (linear, times its strength), w = alpha cutoff (cut-outs)
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance instances[];
};
// Which instance each drawn one is: the identity, except in the main pass's GPU-culled batches (cull.comp)
layout(std430, set = 0, binding = 13) readonly buffer InstanceIndices {
    uint instanceIndex[];
};
layout(std430, set = 0, binding = 3) readonly buffer Joints {
    mat4 joints[];
};

// The instance's model matrix, with the skeleton's deformation for skinned meshes (vertex shaders only: they
// declare inJoints / inWeights at locations 3 and 4)
#define SKINNED_MODEL(inst) \
    ((inst).params.w < 0.0 ? (inst).model : (inst).model * ( \
        inWeights.x * joints[int((inst).params.w) + int(inJoints.x)] + \
        inWeights.y * joints[int((inst).params.w) + int(inJoints.y)] + \
        inWeights.z * joints[int((inst).params.w) + int(inJoints.z)] + \
        inWeights.w * joints[int((inst).params.w) + int(inJoints.w)]))
