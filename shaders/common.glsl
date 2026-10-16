// Shared by the scene shaders: per-frame data (set 0, binding 0) and per-object instance data (set 0, binding 2).
// Must match FrameUBO / InstanceData in src/render/renderer.cpp.

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 shadowViewProj[64];  // 0-3: the sun's cascades, then spot and point lights' views in the shadow atlas
                              // (then split-screen views' cascades; a view's own: 0-3 its cascades, in the atlas)
    vec4 shadowRect[64];      // atlas views: the tile (uv offset, uv size)
    vec4 camPos;
    vec4 fogColor;        // rgb = fog/sky colour, w = density
    vec4 viewport;        // width, height, outline width (px), w = 1: the sun's cascades are atlas tiles
    vec4 sunDir;          // xyz = direction the light travels, w = 1 when it casts shadows
    vec4 sunColor;        // rgb = colour * intensity (linear)
    vec4 ambient;         // rgb = ambient strength, w = number of point and spot lights (the lights buffer)
    vec4 clusterGrid;     // tiles x, y, depth slices; w = 1: clustered (0: every light)
    vec4 clusterDepth;    // near, slices / log(far / near)
    vec4 camRight;        // the camera's axes in the world: billboards face it
    vec4 camUp;
    vec4 camForward;      // w = tan(fovY / 2)
    vec4 cascadeSplits;   // far end of each sun shadow cascade, metres along the view
    vec4 cascadeScale[4]; // per cascade: x = metres across its map, y = metres of depth it spans, z = texels across
    vec4 shadowParams;    // x = the sun's softness, y = 1: shadow taps turned per pixel (with temporal AA),
                          // z = quality (0 low, 1 medium, 2 high: how many taps and steps the lit shader takes),
                          // w = 1: the sun's tint layers hold something (binding 38)
    vec4 fogParams;       // volumetric fog: x = 1 if on, y = light strength, z = anisotropy, w = height falloff
    vec4 fogRange;        // its cells' slices: x = the first's start (m), y = slices / log(far / near), z = far
    mat4 fogPrevViewProj; // the fog's last frame (its cells blend with what they held then): its camera ...
    vec4 fogPrevCam;
    vec4 fogPrevForward;
    vec4 fogPrevRange;    // ... and its slices (w = 1: there is one)
    vec4 zenith;          // sky overhead (linear)
    vec4 ground;          // below the horizon (linear)
    vec4 envParams;       // a sky image: x = 1 if there is one, y = brightness, z = turn around the vertical
    vec4 sh[9];           // its irradiance as spherical harmonics (rgb)
    vec4 probePos[4];     // reflection probes: xyz = where it was baked, w = 1 if used
    vec4 probeMin[4];     // its box
    vec4 probeMax[4];     // w = 1: box projection
    vec4 probeSh[4 * 9];  // its irradiance (nine per probe)
    vec4 time;            // x = seconds (for wind)
    vec4 giOrigin;        // the GI grid (renderer_gi.cpp): xyz = its first probe, w = spacing (0: no GI)
    vec4 giDims;          // probes along x, y, z; w = intensity
    vec4 giCell;          // xyz = the world cell of its first probe (slots wrap around)
    mat4 prevViewProj;    // screen-space reflections: the last frame's camera (its picture: binding 18)
    vec4 prevCamPos;      // w = 1: reflections on
    vec4 prevCamForward;
    vec4 prevViewport;    // the last frame's scene rectangle in its picture (pixels)
    vec4 atmosphere;      // the physically based sky (renderer_sky.cpp): x = 1 if on, y = 1: the ambient from sh
                          // (irradiance), z = 1: the light is the moon, w = cloud cover (0..1)
    vec4 skySun;          // the sun's light above the air (the sky table is for one of it)
    vec4 air;             // the air's extinction per metre (aerial perspective; 0: none)
    vec4 skyToSun;        // towards the real sun (the sky table's: the light, sunDir, may be the moon)
    vec4 weather;         // wetness, puddles, rain (RenderSettings), w = how many decals (binding 24)
    vec4 waterInfo;       // x = 1: refraction (binding 26) this frame, y = how many water boxes
    vec4 waterBox[4];     // each water's rectangle: min x, min z, max x, max z
    vec4 waterLevel[4];   // its level (y) and depth
    vec4 waterTint[4];    // its colour (linear), murk: the view from under its surface (waterInfo.z = 1, .w = which)
    mat4 motionViewProj;      // motion vectors: this frame's camera without temporal AA's jitter ...
    mat4 motionPrevViewProj;  // ... and last frame's
    vec4 motion;              // x = seconds since last frame, y = 1: the prepass writes them
    mat4 invViewProj;         // this frame's (jittered) clip space back to the world: the half-size traces' points
    vec4 occlusion;           // x = 1: ambient occlusion before lighting (binding 37), on the indirect light alone;
                              // y = 1: subsurface pixels' specular kept apart (binding 41) for the diffusion
    vec4 drawnViewport;       // the scene's rectangle as drawn (pixels; smaller with a render scale): the traces'
    vec4 trace;               // x = 1: reflections and GI traced at half size this frame, y = 1: their history is
                              // good, z = which of each 2 x 2 block's pixels they trace this frame (0..3), w = 1:
                              // contact shadows with them
    vec4 contactLight;        // the point or spot light with contact shadows too (the nearest shadowed one): xyz
                              // where, w = its index in the lights (-1: none)
    vec4 shafts;              // light shafts (shafts.glsl): x = scattering per metre (0: none), y = how many lamps, w = steps
    vec4 shaftsViewport;      // the picture's rectangle (pixels: not scaled down), the shafts' half-size pixels' frame
    vec4 shaftLamps;          // the lamps' indices in the lights (shafts.y of them)
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
// The physically based sky's table (sky_view.frag): the sky around the viewer for a sun of irradiance 1. x: the angle
// from the sun around the vertical, y: the elevation, both squeezed where the sky changes most (the sun, the horizon)
layout(set = 0, binding = 23) uniform sampler2D skyTable;
vec2 skyTableUv(vec3 d, vec3 toSun) {
    float elev = asin(clamp(d.y, -1.0, 1.0));
    float coord = sqrt(abs(elev) / (0.5 * kPi));
    float v = elev >= 0.0 ? 0.5 * (1.0 - coord) : 0.5 * (1.0 + coord);
    float a = length(d.xz), b = length(toSun.xz);
    float cosAz = a > 1e-4 && b > 1e-4 ? dot(d.xz / a, toSun.xz / b) : 1.0;
    return vec2(sqrt(clamp(0.5 - 0.5 * cosAz, 0.0, 1.0)), v);
}
// The air's own colour looking along d (no sun disc): the table times the sun's light, starlight at night
vec3 physicalSky(vec3 d) {
    vec3 c = textureLod(skyTable, skyTableUv(d, frame.skyToSun.xyz), 0.0).rgb * frame.skySun.rgb;
    return c + vec3(0.0004, 0.0006, 0.0012) * frame.envParams.y;
}

// The physical sky's extras: stars (where the sky is dark enough to show them), and clouds: a layer 2 km up of two
// octaves of value noise drifting with the wind, as much of it cloud as the cover says
float skyHash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}
vec3 stars(vec3 d) {
    vec3 q = d * 260.0, cell = floor(q);
    float h = skyHash(cell);
    if (h < 0.992) return vec3(0.0);
    vec3 at = cell + 0.5 + (vec3(skyHash(cell + 11.3), skyHash(cell + 23.7), skyHash(cell + 37.1)) - 0.5) * 0.6;
    float r = length(q - at);
    float bright = (h - 0.992) / 0.008;
    return mix(vec3(0.7, 0.8, 1.0), vec3(1.0, 0.9, 0.75), skyHash(cell + 5.0)) * (bright * bright * 0.15) *
           exp(-r * r * 18.0);
}
float cloudNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    float a = skyHash(vec3(i, 1.0)), b = skyHash(vec3(i + vec2(1.0, 0.0), 1.0));
    float c = skyHash(vec3(i + vec2(0.0, 1.0), 1.0)), e = skyHash(vec3(i + vec2(1.0, 1.0), 1.0));
    return mix(mix(a, b, u.x), mix(c, e, u.x), u.y);
}
const float kCloudHeight = 2000.0;   // (metres)
float cloudDensity(vec2 xz) {   // (0..1 at a place on the layer)
    vec2 p = (xz + vec2(9.0, 3.0) * frame.time.x) / 900.0;   // (the wind: 9.5 m/s)
    float n = cloudNoise(p) * 0.6 + cloudNoise(p * 2.7 + 7.1) * 0.3 + cloudNoise(p * 7.3 + 3.3) * 0.1;
    float cover = frame.atmosphere.w;
    return smoothstep(1.0 - cover, 1.0 - cover + 0.25, n) * min(cover * 3.0, 1.0);
}
// How much sunlight the clouds let through at a point (their shadow: where the sun's ray crosses the layer)
float cloudShadow(vec3 pos) {
    if (frame.atmosphere.w <= 0.0 || frame.atmosphere.z > 0.5) return 1.0;
    vec3 L = normalize(frame.skyToSun.xyz);
    if (L.y <= 0.02) return 1.0;
    vec2 at = pos.xz + L.xz / L.y * (kCloudHeight - pos.y);
    return 1.0 - cloudDensity(at) * 0.75;
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
    if (frame.atmosphere.x > 0.5) {   // the air's (below the horizon: towards the ground's colour)
        vec3 d = normalize(dir);
        vec3 c = physicalSky(vec3(d.x, max(d.y, 0.0), d.z));
        float dark = clamp(1.0 - dot(c, vec3(0.3, 0.5, 0.2)) * 60.0, 0.0, 1.0);   // (the stars show in a dark sky)
        if (d.y > 0.0) c += stars(d) * dark * frame.envParams.y;
        if (withSun) {
            float disc = smoothstep(0.9995, 0.9998, max(dot(d, -frame.sunDir.xyz), 0.0));
            // (the light's disc: the sun's, or the moon's: pale, its own brightness)
            c += frame.atmosphere.z > 0.5 ? vec3(0.5, 0.52, 0.55) * disc * frame.envParams.y : frame.sunColor.rgb * disc * 8.0;
        }
        if (frame.atmosphere.w > 0.0 && d.y > 0.0) {   // clouds: lit by the sun through the air, and by the sky
            float t = kCloudHeight / max(d.y, 0.03);
            float density = cloudDensity(frame.camPos.xz + d.xz * t);
            vec3 L = normalize(frame.skyToSun.xyz);
            float forward = pow(max(dot(d, L), 0.0), 8.0);   // (bright edges looking towards the sun)
            vec3 lit = frame.atmosphere.z > 0.5 ? frame.sunColor.rgb * 0.5 : frame.sunColor.rgb * (0.12 + forward * 0.5);
            vec3 cloud = lit * (1.0 - density * 0.5) + (frame.zenith.rgb + frame.fogColor.rgb) * 0.6;
            c = mix(c, cloud, density * smoothstep(0.0, 0.12, d.y));   // (thinning to the horizon)
        }
        return c;
    }
    vec3 horizon = frame.fogColor.rgb;
    vec3 c = dir.y >= 0.0 ? mix(horizon, frame.zenith.rgb, pow(clamp(dir.y, 0.0, 1.0), 0.6))
                          : mix(horizon, frame.ground.rgb, pow(clamp(-dir.y * 4.0, 0.0, 1.0), 0.5));
    if (withSun) {
        float s = max(dot(dir, -frame.sunDir.xyz), 0.0);
        c += frame.sunColor.rgb * (smoothstep(0.9995, 0.9998, s) * 8.0 + pow(s, 12.0) * 0.12);
    }
    return c;
}

// The view from under a water's surface (frame.waterInfo.z = 1): what's at `pos` seen through the water between,
// the colours the water's colour lacks absorbed first, the rest filled with its colour, lit from above and darker the
// deeper the camera is (only the part of the way under the surface counts)
vec3 throughWater(vec3 color, vec3 pos) {
    int i = int(frame.waterInfo.w + 0.5);
    float level = frame.waterLevel[i].x;
    vec3 cam = frame.camPos.xyz, v = pos - cam;
    float dist = length(v), inWater = dist;
    if (pos.y > level && v.y > 1e-4) inWater = dist * clamp((level - cam.y) / v.y, 0.0, 1.0);
    vec4 t = frame.waterTint[i];
    vec3 k = (vec3(1.0) - t.rgb) * (0.15 + 1.6 * t.a) + 0.02;
    vec3 through = exp(-k * inWater);
    vec3 above = frame.sunColor.rgb * max(-frame.sunDir.y, 0.0) * 0.15 + mix(frame.fogColor.rgb, frame.zenith.rgb, 0.6) * frame.ambient.rgb;
    vec3 body = t.rgb * above * exp(-k * max(level - cam.y, 0.0) * 0.5);
    return color * through + body * (1.0 - through);
}

// packColor's (renderer_internal.h) colour back, linear
vec3 unpackColor(float packed) {
    float p = floor(packed + 0.5);
    vec3 srgb = vec3(floor(p / 65536.0), mod(floor(p / 256.0), 256.0), mod(p, 256.0)) / 255.0;
    return pow(srgb, vec3(2.2));
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
    vec4 stream;   // texture streaming: x = the texture whose detail to report (-1: none), y = log2 of its size
    vec4 layers;   // clear coat, its roughness, sheen, anisotropy
    vec4 surface;  // parallax depth (0: none), detail tiling, detail strength (0: none), w = 1: sheltered from weather
    mat4 prevModel;  // last frame's model matrix (motion vectors)
    vec4 motion;     // x = last frame's joints' first matrix (-1: none)
    vec4 scatter;    // translucency, subsurface, its colour (8-bit sRGB packed: r * 65536 + g * 256 + b), unused
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
// The same with last frame's model and joints (motion vectors)
#define PREV_SKINNED_MODEL(inst) \
    ((inst).motion.x < 0.0 ? (inst).prevModel : (inst).prevModel * ( \
        inWeights.x * joints[int((inst).motion.x) + int(inJoints.x)] + \
        inWeights.y * joints[int((inst).motion.x) + int(inJoints.y)] + \
        inWeights.z * joints[int((inst).motion.x) + int(inJoints.z)] + \
        inWeights.w * joints[int((inst).motion.x) + int(inJoints.w)]))
#define SKINNED_MODEL(inst) \
    ((inst).params.w < 0.0 ? (inst).model : (inst).model * ( \
        inWeights.x * joints[int((inst).params.w) + int(inJoints.x)] + \
        inWeights.y * joints[int((inst).params.w) + int(inJoints.y)] + \
        inWeights.z * joints[int((inst).params.w) + int(inJoints.z)] + \
        inWeights.w * joints[int((inst).params.w) + int(inJoints.w)]))
