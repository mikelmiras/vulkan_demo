// Shared by the scene shaders: per-frame data (set 0, binding 0) and per-object instance data (set 0, binding 2).
// Must match FrameUBO / InstanceData in src/render/renderer.cpp.

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 shadowViewProj[8];   // 0-3: the sun's cascades, 4-7: spot lights
    vec4 camPos;
    vec4 fogColor;        // rgb = fog/sky colour, w = density
    vec4 viewport;        // width, height, outline width (px), unused
    vec4 sunDir;          // xyz = direction the light travels, w = 1 when it casts shadows
    vec4 sunColor;        // rgb = colour * intensity (linear)
    vec4 ambient;         // rgb = ambient strength, w = number of point lights
    vec4 pointPos[8];     // xyz = position, w = range
    vec4 pointColor[8];   // rgb = colour * intensity (linear)
    vec4 pointDir[8];     // spot lights: xyz = the direction it shines
    vec4 pointSpot[8];    // x = cos inner, y = cos outer (-2: a point light), z = shadow layer (4-7) or -1
    vec4 camRight;        // the camera's axes in the world: billboards face it
    vec4 camUp;
    vec4 camForward;      // w = tan(fovY / 2)
    vec4 cascadeSplits;   // far end of each sun shadow cascade, metres along the view
    vec4 zenith;          // sky overhead (linear)
    vec4 ground;          // below the horizon (linear)
} frame;

// The sky in a direction: horizon (the fog colour) to zenith above, to ground below; with the sun, its disc and glow
vec3 skyColor(vec3 dir, bool withSun) {
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
    vec4 material; // x = metallic, y = roughness, z = normal map strength (0: no normal map)
    vec4 emissive; // rgb = emitted light (linear, times its strength)
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance instances[];
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
