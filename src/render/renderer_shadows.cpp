// Shadows: the sun's 4 cascades (each an orthographic camera around a slice of the view, snapped to texels so edges
// don't shimmer) and up to 4 spot lights (perspective), rendered depth-only into two layered images.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>

#include "render/renderer_internal.h"

using namespace render_detail;

Renderer::ShadowArray Renderer::createShadowArray(uint32_t size) {
    ShadowArray a;
    a.size = size;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = shadowFormat_;
    ci.extent = {size, size, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 4;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &a.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, a.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &a.memory));
    VK_CHECK(vkBindImageMemory(device_, a.image, a.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = a.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = shadowFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 4};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &a.array));
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    for (uint32_t l = 0; l < 4; ++l) {
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, l, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &a.layers[l]));
    }
    return a;
}

void Renderer::destroyShadowArray(ShadowArray& a) {
    for (VkImageView v : a.layers) vkDestroyImageView(device_, v, nullptr);
    vkDestroyImageView(device_, a.array, nullptr);
    vkDestroyImage(device_, a.image, nullptr);
    vkFreeMemory(device_, a.memory, nullptr);
    a = {};
}

uint32_t Renderer::computeShadows(const SceneRenderInfo& info, Vec3 sunDir, bool sunShadows,
                                  const std::vector<const LightInfo*>& spots, FrameUBO& u) const {
    uint32_t layers = 0;
    // ---- The sun: the view from 0.1 m to kShadowDistance in 4 slices, closer ones smaller (sharper)
    Vec3 fwd = normalize(info.camForward);
    Vec3 right = std::fabs(fwd.y) > 0.999f ? Vec3{1, 0, 0} : normalize(cross(fwd, {0, 1, 0}));
    Vec3 up = cross(right, fwd);
    float tanY = std::tan(info.fovY * 0.5f), tanX = tanY * std::max(info.aspect, 0.01f);
    const float n = 0.1f, f = kShadowDistance;
    float prev = n;
    for (uint32_t c = 0; c < kCascades; ++c) {
        float t = (float)(c + 1) / (float)kCascades;
        float split = 0.8f * n * std::pow(f / n, t) + 0.2f * (n + (f - n) * t);   // mostly logarithmic
        u.cascadeSplits[c] = split;
        if (sunShadows) {
            // A sphere around the slice (rotating the view doesn't change its size), then an orthographic camera
            // looking down the sun around it, with room behind for casters between the sun and the slice
            Vec3 center = info.camPos + fwd * ((prev + split) * 0.5f);
            float r = 0.f;
            for (float d : {prev, split})
                for (float sx : {-1.f, 1.f})
                    for (float sy : {-1.f, 1.f}) {
                        Vec3 corner = info.camPos + fwd * d + right * (sx * d * tanX) + up * (sy * d * tanY);
                        r = std::max(r, length(corner - center));
                    }
            r = std::ceil(r * 16.f) / 16.f;
            const float behind = 120.f;
            Vec3 sunUp = std::fabs(sunDir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            Mat4 view = lookAt(center - sunDir * (r + behind), center, sunUp);
            Mat4 proj = orthographic(-r, r, -r, r, 0.1f, 2.f * r + behind);
            Mat4 vp = proj * view;
            float half = kCascadeSize * 0.5f;
            float ox = vp.m[12] * half, oy = vp.m[13] * half;   // where the world origin lands, in texels
            proj.m[12] += (std::round(ox) - ox) / half;
            proj.m[13] += (std::round(oy) - oy) / half;
            u.shadowViewProj[c] = proj * view;
            layers |= 1u << c;
        }
        prev = split;
    }
    // ---- Spot lights (the nearest ones casting shadows): a perspective camera down the cone
    uint32_t slot = 0;
    for (const LightInfo* l : spots) {
        if (slot >= kSpotShadows) break;
        if (!l->castShadows) continue;
        Vec3 dir = normalize(l->direction);
        Vec3 lup = std::fabs(dir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        float fov = std::min(2.f * l->spotAngle * DEG2RAD * 1.1f, 3.f);
        u.shadowViewProj[kCascades + slot] =
            perspective(fov, 1.f, 0.1f, std::max(l->range, 0.2f), true) * lookAt(l->position, l->position + dir, lup);
        layers |= 1u << (kCascades + slot);
        ++slot;
    }
    return layers;
}

void Renderer::recordShadows(VkCommandBuffer cb) {
    const VkPipelineStageFlags depthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                       VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 4};
        vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    if (!shadowLayers_) return;   // (both stay readable, as last frame left them)
    // The previous frame's sampling must finish first; the old contents are discarded
    for (ShadowArray* a : {&cascades_, &spotShadows_})
        barrier(a->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, depthStages,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    for (uint32_t layer = 0; layer < kCascades + kSpotShadows; ++layer) {
        const ShadowArray& a = layer < kCascades ? cascades_ : spotShadows_;
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = a.layers[layer % 4];
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // (every layer: unused ones read as "lit")
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {1.f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {a.size, a.size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cb, &ri);
        if (shadowLayers_ & (1u << layer)) {
            VkViewport vp{0.f, 0.f, (float)a.size, (float)a.size, 0.f, 1.f};
            VkRect2D sc{{0, 0}, {a.size, a.size}};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            int32_t index = (int32_t)layer;
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(index), &index);
            for (const Batch& b : shadowBatches_[layer]) {
                const Mesh& mesh = meshes_->get(b.mesh);
                vkCmdDrawIndexed(cb, mesh.indexCount, b.count, mesh.firstIndex, mesh.vertexOffset, b.first);
            }
        }
        vkCmdEndRendering(cb);
    }
    for (ShadowArray* a : {&cascades_, &spotShadows_})
        barrier(a->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
}
