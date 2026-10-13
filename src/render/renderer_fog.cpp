// Volumetric fog: two compute passes before the scene (fog_inject.comp: each cell's density and in-scattered light;
// fog_integrate.comp: summed from the camera out) into a 3D image per frame in flight, which scene.frag and sky.frag
// read (binding 21). Main view only: extra views and probe faces keep the plain fog.
#include "render/renderer.h"

#include <algorithm>

#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

constexpr uint32_t kFogX = 160, kFogY = 90, kFogZ = 64;   // (fog.glsl's kFogCells)

}  // namespace

Renderer::Image Renderer::createVolume() {
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {kFogX, kFogY, kFogZ};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(device_, img.image, img.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = ci.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &img.view));
    return img;
}

void Renderer::createFog() {
    for (FogImages& f : fog_) {
        f.scatter = createVolume();
        f.volume = createVolume();
    }
    // The volumes start out readable (frames without fog still bind them), as the scene passes expect them
    VkCommandBuffer cb = beginOneTimeCommands();
    for (FogImages& f : fog_) {
        imageBarrier(cb, f.scatter.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);
        imageBarrier(cb, f.volume.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    endOneTimeCommands(cb);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &fogSampler_));

    // Set 1 of the compute passes: the two volumes as storage images (set 0 is the frame's, as the scene's)
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &fogSetLayout_));
    VkDescriptorSetLayout layouts[2] = {setLayout_, fogSetLayout_};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = layouts;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &fogLayout_));
    for (auto [shader, pipeline] : {std::pair{"fog_inject.comp.spv", &fogInjectPipeline_},
                                    std::pair{"fog_integrate.comp.spv", &fogIntegratePipeline_}}) {
        VkShaderModule cs = loadShader(shader);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main",
                    nullptr};
        ci.layout = fogLayout_;
        VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, pipeline));
        vkDestroyShaderModule(device_, cs, nullptr);
    }
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * MAX_FRAMES};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &fogPool_));
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = fogPool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &fogSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &fogSets_[i]));
        VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, fog_[i].scatter.view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, fog_[i].volume.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (uint32_t k = 0; k < 2; ++k) {
            w[k].dstSet = fogSets_[i];
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[k].pImageInfo = &infos[k];
        }
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
}

void Renderer::destroyFog() {
    for (FogImages& f : fog_) {
        destroyImage(f.scatter);
        destroyImage(f.volume);
    }
    vkDestroyDescriptorPool(device_, fogPool_, nullptr);
    vkDestroyPipeline(device_, fogInjectPipeline_, nullptr);
    vkDestroyPipeline(device_, fogIntegratePipeline_, nullptr);
    vkDestroyPipelineLayout(device_, fogLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, fogSetLayout_, nullptr);
    vkDestroySampler(device_, fogSampler_, nullptr);
}

void Renderer::recordFog(VkCommandBuffer cb) {
    if (!fogOn_) return;
    FogImages& f = fog_[frame_];
    const VkPipelineStageFlags readers = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    // (the last time this frame index's volume was read: by fragment shaders; the scatter by the integration)
    imageBarrier(cb, f.scatter.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT);
    imageBarrier(cb, f.volume.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL, readers, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    uint32_t slot = 0;   // (the main view's frame data)
    VkDescriptorSet sets[2] = {descSets_[frame_], fogSets_[frame_]};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fogLayout_, 0, 2, sets, 1, &slot);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fogInjectPipeline_);
    vkCmdDispatch(cb, (kFogX + 7) / 8, (kFogY + 7) / 8, kFogZ);
    imageBarrier(cb, f.scatter.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fogIntegratePipeline_);
    vkCmdDispatch(cb, (kFogX + 7) / 8, (kFogY + 7) / 8, 1);
    imageBarrier(cb, f.volume.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 readers, VK_ACCESS_SHADER_READ_BIT);
}
