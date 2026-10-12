// Shared by the renderer's source files (renderer*.cpp): the frame and instance data the shaders read, and small
// Vulkan helpers. Not part of the engine's interface.
#pragma once

#include <vulkan/vulkan.h>

#include <cmath>
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
constexpr int kShadowViews = 52;       // 4 sun cascades + 48 views of spot and point lights in the atlas
constexpr int kMaxLights = 1024;       // point and spot lights drawn (the nearest)
constexpr int kClusterX = 16, kClusterY = 9, kClusterSlices = 24;
constexpr size_t kMaxClusterIndices = 1 << 17;
struct FrameUBO {
    Mat4 viewProj;
    Mat4 shadowViewProj[kShadowViews];  // 0-3: the sun's cascades, then views in the shadow atlas
    float shadowRect[kShadowViews][4];  // atlas views: the tile (uv offset, uv size)
    float camPos[4];
    float fogColor[4];   // rgb linear, w = density
    float viewport[4];   // width, height, outline width in pixels, unused
    float sunDir[4];     // xyz, w = 1 if it casts shadows
    float sunColor[4];   // rgb linear * intensity
    float ambient[4];    // rgb, w = point and spot light count (the lights buffer)
    float clusterGrid[4];    // tiles x, y, depth slices, w = 1: clustered (0: every light, for probe views)
    float clusterDepth[4];   // near, slices / log(far / near)
    float camRight[4], camUp[4];
    float camForward[4];      // w = tan(fovY / 2)
    float cascadeSplits[4];   // far end of each cascade, metres along the view
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
    float emissive[4];   // rgb (linear, times strength), unused
};

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
