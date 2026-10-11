// Textures (an sRGB and a UNORM view each), material descriptor sets, and textures for Dear ImGui.
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

#include "imgui_impl_vulkan.h"
#include "render/renderer_internal.h"

using namespace render_detail;

// Uploads RGBA8 sRGB pixels to a device-local image with a full mip chain (when the format can be blitted)
Renderer::GpuTexture Renderer::createTexture(const uint8_t* rgba, uint32_t width, uint32_t height, VkCommandBuffer frame) {
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu_, format, &fp);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    uint32_t mips = 1;
    if ((fp.optimalTilingFeatures & blit) == blit)
        while ((std::max(width, height) >> mips) > 0) ++mips;

    GpuTexture t;
    // Mutable format: an sRGB view for colour and a UNORM view of the same bytes for data (normal maps)
    t.image = createImage(format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, width, height, VK_SAMPLE_COUNT_1_BIT, mips,
                          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    t.linear = createView(t.image.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, mips);
    Buffer staging = createBuffer((VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba);

    auto barrier = [&](VkCommandBuffer cb, uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                       VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    VkCommandBuffer cb = frame ? frame : beginOneTimeCommands();
    barrier(cb, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int32_t w = (int32_t)width, h = (int32_t)height;
    for (uint32_t m = 1; m < mips; ++m) {   // each level is a filtered half-size copy of the previous one
        barrier(cb, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        region.srcOffsets[1] = {w, h, 1};
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        region.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cb, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        barrier(cb, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    barrier(cb, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    if (frame) {
        retired_[frame_].push_back(staging);
    } else {
        endOneTimeCommands(cb);
        destroyBuffer(staging);
    }
    return t;
}

void Renderer::destroyTexture(GpuTexture& t) {
    if (t.linear) vkDestroyImageView(device_, t.linear, nullptr);
    destroyImage(t.image);   // the descriptor set goes with its pool
    t = {};
}

void Renderer::uploadTextures(VkCommandBuffer frame) {
    const size_t kFrameBudget = 24u << 20;   // bytes of pixels per frame (at least one texture)
    size_t sent = 0;
    while ((int)gpuTextures_.size() < textures_->count() && (!frame || sent < kFrameBudget)) {
        const Texture& tex = textures_->get((int)gpuTextures_.size());
        gpuTextures_.push_back(createTexture(tex.rgba.data(), tex.width, tex.height, frame));
        sent += tex.rgba.size();
    }
    // The shadow map starts out readable, so frames without shadows can still bind it
    if (!shadowMapReady_) {
        VkCommandBuffer cb = beginOneTimeCommands();
        for (VkImage image : {cascades_.image, spotShadows_.image, pointShadows_.image}) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        }
        endOneTimeCommands(cb);
        shadowMapReady_ = true;
    }
}

VkDescriptorSet Renderer::materialSet(const DrawItem& o) {
    auto valid = [&](int id) { return id >= 0 && id < (int)gpuTextures_.size() ? id : -1; };
    int ids[4] = {valid(o.texture), valid(o.normalTexture), valid(o.metalRoughTexture), valid(o.emissiveTexture)};
    uint64_t key = 0;
    for (int id : ids) key = (key << 16) | (uint64_t)(uint16_t)(id + 1);
    auto it = materialSets_.find(key);
    if (it != materialSets_.end()) return it->second;

    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = materialPool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &materialSetLayout_;
    if (vkAllocateDescriptorSets(device_, &dsai, &set) != VK_SUCCESS) {   // out of sets: reuse the plainest one
        return materialSets_.empty() ? VK_NULL_HANDLE : materialSets_.begin()->second;
    }
    auto pick = [&](int id, bool colour, const GpuTexture& fallback) {
        const GpuTexture& t = id >= 0 ? gpuTextures_[(size_t)id] : fallback;
        return VkDescriptorImageInfo{sampler_, colour ? t.image.view : t.linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    VkDescriptorImageInfo infos[4] = {pick(ids[0], true, whiteTexture_), pick(ids[1], false, flatNormal_),
                                      pick(ids[2], false, whiteTexture_), pick(ids[3], true, whiteTexture_)};
    VkWriteDescriptorSet w[4];
    for (uint32_t i = 0; i < 4; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
    materialSets_.emplace(key, set);
    return set;
}

ImTextureID Renderer::uiTexture(int id) const {
    // The ImGui backend takes its own kind of descriptor set (a sampled image; it brings the sampler): made for each
    // texture the first time the UI shows it, from the UNORM view (the UI's colours are already as they should look)
    if (id < 0 || id >= (int)gpuTextures_.size() || !imguiInitialized_) return (ImTextureID)0;
    auto it = uiSets_.find(id);
    if (it == uiSets_.end())
        it = uiSets_.emplace(id, ImGui_ImplVulkan_AddTexture(gpuTextures_[(size_t)id].linear,
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)).first;
    return (ImTextureID)it->second;
}
