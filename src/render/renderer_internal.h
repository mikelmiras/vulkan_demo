// Shared by the renderer's source files (renderer*.cpp): the frame and instance data the shaders read, and small
// Vulkan helpers. Not part of the engine's interface.
#pragma once

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/vecmath.h"

#define VK_CHECK(expr)                                                                         \
    do {                                                                                       \
        VkResult r_ = (expr);                                                                  \
        if (r_ != VK_SUCCESS) {                                                                \
            std::fprintf(stderr, "Vulkan error %d at %s:%d\n  %s\n", r_, __FILE__, __LINE__, #expr); \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)

namespace render_detail {

// Must match shaders/common.glsl
constexpr int kMaxPointLights = 8;
struct FrameUBO {
    Mat4 viewProj;
    Mat4 shadowViewProj[8];   // 0-3: the sun's cascades, 4-7: spot lights
    float camPos[4];
    float fogColor[4];   // rgb linear, w = density
    float viewport[4];   // width, height, outline width in pixels, unused
    float sunDir[4];     // xyz, w = 1 if it casts shadows
    float sunColor[4];   // rgb linear * intensity
    float ambient[4];    // rgb, w = point light count
    float pointPos[kMaxPointLights][4];     // xyz, w = range
    float pointColor[kMaxPointLights][4];   // rgb linear * intensity
    float pointDir[kMaxPointLights][4];     // spot lights: xyz = direction it shines
    float pointSpot[kMaxPointLights][4];    // x = cos inner, y = cos outer (-2: a point light), z = shadow layer or -1
    float camRight[4], camUp[4];
    float camForward[4];      // w = tan(fovY / 2)
    float cascadeSplits[4];   // far end of each cascade, metres along the view
    float zenith[4], ground[4];   // sky colours (linear)
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
