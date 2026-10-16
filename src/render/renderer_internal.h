// Shared by the renderer's source files (renderer*.cpp): the frame and instance data the shaders read, and small
// Vulkan helpers. Not part of the engine's interface.
#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core/log.h"
#include "core/vecmath.h"

#define VK_CHECK(expr)                                                                         \
    do {                                                                                       \
        VkResult r_ = (expr);                                                                  \
        if (r_ != VK_SUCCESS) {                                                                \
            logLine("fatal: Vulkan error %d at %s:%d: %s", r_, __FILE__, __LINE__, #expr);    \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)

namespace render_detail {

// Must match shaders/common.glsl
constexpr int kShadowViews = 64;       // 4 sun cascades, 44 views of spot and point lights in the atlas, then the
                                       // extra views' (split screen's) 4 cascades each, in the atlas too
constexpr int kMaxLights = 1024;       // point and spot lights drawn (the nearest)
constexpr int kClusterX = 16, kClusterY = 9, kClusterSlices = 24;
constexpr size_t kMaxClusterIndices = 1 << 17;
struct FrameUBO {
    Mat4 viewProj;
    Mat4 shadowViewProj[kShadowViews];  // 0-3: the sun's cascades, then views in the shadow atlas (an extra view's
                                        // own: 0-3 its cascades, in the atlas)
    float shadowRect[kShadowViews][4];  // atlas views: the tile (uv offset, uv size)
    float camPos[4];
    float fogColor[4];   // rgb linear, w = density
    float viewport[4];   // width, height, outline width in pixels, w = 1: the sun's cascades are atlas tiles
    float sunDir[4];     // xyz, w = 1 if it casts shadows
    float sunColor[4];   // rgb linear * intensity
    float ambient[4];    // rgb, w = point and spot light count (the lights buffer)
    float clusterGrid[4];    // tiles x, y, depth slices, w = 1: clustered (0: every light, for probe views)
    float clusterDepth[4];   // near, slices / log(far / near)
    float camRight[4], camUp[4];
    float camForward[4];      // w = tan(fovY / 2)
    float cascadeSplits[4];   // far end of each cascade, metres along the view
    float cascadeScale[4][4]; // per cascade: x = metres across its map, y = metres of depth it spans, z = texels across
    float shadowParams[4];    // x = the sun's softness, y = 1: shadow taps turned per pixel (with temporal AA),
                              // z = quality (0 low, 1 medium, 2 high: how many taps and steps the lit shader takes)
    float fogParams[4];       // volumetric fog: x = 1 if on, y = light strength, z = anisotropy, w = height falloff
    float fogRange[4];        // its cells' slices: x = the first's start (m), y = slices / log(far / near), z = far
    // The fog's last frame (its cells blend with what they held then): its camera, its slices (w = 1: there is one)
    Mat4 fogPrevViewProj;
    float fogPrevCam[4];      // xyz = where its camera was
    float fogPrevForward[4];
    float fogPrevRange[4];    // x = its first slice's start, y = slices / log(far / near), w = 1 if valid
    float zenith[4], ground[4];   // sky colours (linear)
    float envParams[4];       // the sky image: x = 1 if there is one, y = brightness, z = turn (radians)
    float sh[9][4];           // its irradiance (spherical harmonics, rgb)
    float probePos[4][4];     // reflection probes: xyz = where it was baked, w = 1 if used
    float probeMin[4][4];     // its box
    float probeMax[4][4];     // w = 1: box projection
    float probeSh[4][9][4];   // its irradiance
    float time[4];            // x = seconds since the renderer started (wind)
    float giOrigin[4];        // the GI grid: xyz = its first probe, w = spacing (0: no GI)
    float giDims[4];          // probes along x, y, z; w = intensity
    float giCell[4];          // xyz = the world cell of its first probe (slots wrap around: renderer_gi.cpp)
    // Screen-space reflections: the last frame's camera (its picture is binding 18)
    Mat4 prevViewProj;
    float prevCamPos[4];      // w = 1: reflections on (only this view's: the picture is its own)
    float prevCamForward[4];
    float prevViewport[4];    // the last frame's scene rectangle in its picture (pixels)
    // The physically based sky (renderer_sky.cpp)
    float atmosphere[4];      // x = 1: on (the sky table at binding 23), y = 1: the ambient from sh, z = 1: the light is
                              // the moon, w = cloud cover
    float skySun[4];          // the sun's light above the air (rgb: the sky table is for one of it)
    float air[4];             // the air's extinction per metre (rgb: aerial perspective; 0: none)
    float skyToSun[4];        // towards the real sun (the sky tables'; the light may be the moon)
    float weather[4];         // wetness, puddles, rain, w = how many decals (binding 24)
    // Water: x = 1: refraction (binding 26) this frame, y = how many water boxes, z = 1: the camera's under the
    // surface of box w; each box (its rectangle, x and z)
    // and its level and depth (caustics on what's under it)
    float waterInfo[4];
    float waterBox[4][4];
    float waterLevel[4][4];
    float waterTint[4][4];    // its colour (linear) and murk (the underwater view)
    // Motion vectors: this frame's camera and last frame's, both without temporal AA's jitter; x = seconds since last
    // frame (the waves' motion), y = 1: the prepass writes them
    Mat4 motionViewProj;
    Mat4 motionPrevViewProj;
    float motion[4];
    // Reflections and GI traced at half size (renderer_trace.cpp)
    Mat4 invViewProj;         // this frame's clip space (jittered) back to the world
    float occlusion[4];       // x = 1: ambient occlusion before lighting (binding 37: scene.frag's indirect light)
    float drawnViewport[4];   // the scene's rectangle as drawn (pixels: smaller with a render scale): the traces'
    float trace[4];           // x = 1: traced this frame, y = 1: their history is good, z = the 2 x 2 block's pixel,
                              // w = 1: contact shadows (in tracedDepth_'s g)
    float contactLight[4];    // the lamp with contact shadows too (tracedDepth_'s b): xyz where, w = its index (-1: none)
};

// A decal (set 0, binding 24): render/decals.h picks them
struct GpuDecal {
    Mat4 toBox;          // world -> its box (-0.5..0.5; projecting down its -y)
    float color[4];      // rgb linear, a = opacity
    float params[4];     // x = texture slot (binding 25; -1: none), y = roughness (-1: as it is), z = wetness, w = angle fade
    float down[4];       // its direction in the world
    float sphere[4];     // round its box: centre, radius squared (a quick test before the box's)
    float normal[4];     // x = normal map slot (binding 28; -1: none), y = its strength, z = its glow (0: none)
};

// A point or spot light (set 0, binding 14)
struct GpuLight {
    float posRange[4];   // xyz, w = range
    float color[4];      // rgb linear * intensity, w = 1: spot
    float dir[4];        // spots: the direction it shines, w = cos outer
    float params[4];     // x = cos inner, y = first shadow view (-1: none), z = cookie layer (-1: none), w = tan outer
};

struct InstanceData {
    Mat4 model;
    float color[4];      // rgb (linear albedo; sRGB for the outline)
    float params[4];     // x = checker ground, y = texture tiling, z = textured, w = first joint matrix or -1
    float material[4];   // metallic, roughness, normal map strength (0: none), unused
    float emissive[4];   // rgb (linear, times strength), w = alpha cutoff
    float stream[4];     // texture streaming: x = the texture to report the finest level wanted of (-1: none),
                         // y = log2 of its larger side (texture_streaming.h)
    float layers[4];     // clear coat, its roughness, sheen, anisotropy
    float surface[4];    // parallax depth (0: none), detail tiling, detail strength (0: none), w = 1: sheltered
    Mat4 prevModel;      // last frame's model matrix (motion vectors)
    float motion[4];     // x = last frame's joints' first matrix (-1: none)
    float scatter[4];    // translucency, subsurface, its colour (8-bit sRGB packed: r * 65536 + g * 256 + b), unused
};

// An sRGB colour (0..1) as one float, exactly: 8 bits a channel, r * 65536 + g * 256 + b (under 2^24, so a float
// holds it whole); common.glsl's unpackColor undoes it (to linear)
inline float packColor(Vec3 srgb) {
    auto byte = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    return (float)(byte(srgb.x) * 65536u + byte(srgb.y) * 256u + byte(srgb.z));
}

// The six planes of a view-projection's frustum (Vulkan clip space: 0 <= z <= w), as (normal, distance)
struct Frustum {
    float p[6][4];
    explicit Frustum(const Mat4& vp) {
        auto row = [&](int r, float out[4]) { for (int c = 0; c < 4; ++c) out[c] = vp.m[c * 4 + r]; };
        float r0[4], r1[4], r2[4], r3[4];
        row(0, r0); row(1, r1); row(2, r2); row(3, r3);
        for (int k = 0; k < 4; ++k) {
            p[0][k] = r3[k] + r0[k];
            p[1][k] = r3[k] - r0[k];
            p[2][k] = r3[k] + r1[k];
            p[3][k] = r3[k] - r1[k];
            p[4][k] = r2[k];
            p[5][k] = r3[k] - r2[k];
        }
    }
    bool visible(Vec3 centre, float radius) const {   // (a sphere: at least partly inside?)
        for (const auto& pl : p) {
            float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
            if (pl[0] * centre.x + pl[1] * centre.y + pl[2] * centre.z + pl[3] < -radius * len) return false;
        }
        return true;
    }
    // Is the mesh's bounding box, placed by `model`, at least partly inside?
    bool visible(const Mat4& model, Vec3 lo, Vec3 hi) const {
        Vec3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
        Vec3 wc = transformPoint(model, c);
        // World-space half extents of the transformed box
        Vec3 we{std::fabs(model.m[0]) * e.x + std::fabs(model.m[4]) * e.y + std::fabs(model.m[8]) * e.z,
                std::fabs(model.m[1]) * e.x + std::fabs(model.m[5]) * e.y + std::fabs(model.m[9]) * e.z,
                std::fabs(model.m[2]) * e.x + std::fabs(model.m[6]) * e.y + std::fabs(model.m[10]) * e.z};
        for (const auto& pl : p) {
            float d = pl[0] * wc.x + pl[1] * wc.y + pl[2] * wc.z + pl[3];
            float r = std::fabs(pl[0]) * we.x + std::fabs(pl[1]) * we.y + std::fabs(pl[2]) * we.z;
            if (d + r < 0.f) return false;
        }
        return true;
    }
};

inline void set4(float out[4], Vec3 v, float w) {
    out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = w;
}

inline constexpr Vec3 kOutlineSrgb{1.0f, 0.55f, 0.1f};

inline void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                  VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

inline void mipBarrier(VkCommandBuffer cb, VkImage image, uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace render_detail
