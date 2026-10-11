// Shared by the scene shaders: per-frame data (set 0, binding 0) and per-object instance data (set 0, binding 2).
// Must match FrameUBO / InstanceData in src/render/renderer.cpp.

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 lightViewProj;   // the directional light's shadow camera
    vec4 camPos;
    vec4 fogColor;        // rgb = fog/sky colour, w = density
    vec4 viewport;        // width, height, outline width (px), unused
    vec4 sunDir;          // xyz = direction the light travels, w = 1 when it casts shadows
    vec4 sunColor;        // rgb = colour * intensity (linear)
    vec4 ambient;         // rgb = ambient strength, w = number of point lights
    vec4 pointPos[8];     // xyz = position, w = range
    vec4 pointColor[8];   // rgb = colour * intensity (linear)
    vec4 camRight;        // the camera's axes in the world: billboards face it
    vec4 camUp;
} frame;

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
