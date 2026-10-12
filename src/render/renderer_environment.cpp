// The sky image and its prefiltered radiance (render/environment.h) on the GPU: set 0, bindings 7 and 8.
#include "render/renderer.h"

#include <cstring>

#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return (uint16_t)sign;                       // (tiny: zero)
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);          // (huge: the largest half)
    uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h;                                   // round to nearest
    return (uint16_t)h;
}

}  // namespace

// RGBA16F, one mip level per given level (each half the size of the one before)
Renderer::Image Renderer::createFloatImage(const std::vector<const Environment::Level*>& levels) {
    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    uint32_t mips = (uint32_t)levels.size();
    Image img = createImage(format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                            levels[0]->width, levels[0]->height, VK_SAMPLE_COUNT_1_BIT, mips);
    std::vector<uint16_t> pixels;
    std::vector<VkBufferImageCopy> copies;
    for (uint32_t m = 0; m < mips; ++m) {
        const Environment::Level& l = *levels[m];
        VkBufferImageCopy c{};
        c.bufferOffset = pixels.size() * sizeof(uint16_t);
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        c.imageExtent = {l.width, l.height, 1};
        copies.push_back(c);
        for (size_t i = 0; i < (size_t)l.width * l.height; ++i) {
            for (int k = 0; k < 3; ++k) pixels.push_back(toHalf(l.rgb[i * 3 + (size_t)k]));
            pixels.push_back(toHalf(1.f));
        }
    }
    Buffer staging = createBuffer(pixels.size() * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, pixels.data());
    VkCommandBuffer cb = beginOneTimeCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdCopyBufferToImage(cb, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(),
                           copies.data());
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    endOneTimeCommands(cb);
    destroyBuffer(staging);
    return img;
}

void Renderer::writeEnvironmentDescriptors() {
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorImageInfo sky{envSampler_, envSky_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo radiance{envSampler_, envRadiance_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (int k = 0; k < 2; ++k) {
            w[k].dstSet = descSets_[i];
            w[k].dstBinding = 7 + (uint32_t)k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = k == 0 ? &sky : &radiance;
        }
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
}

void Renderer::createEnvironmentResources() {
    // Around the sky: wraps left to right, stops at the poles; between the radiance levels
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &envSampler_));
    // Until a scene has a sky image: a black one (the shaders don't read it then)
    Environment::Level black;
    black.width = black.height = 1;
    black.rgb = {0.f, 0.f, 0.f};
    envSky_ = createFloatImage({&black});
    envRadiance_ = createFloatImage({&black});
    envId_ = 0;
    writeEnvironmentDescriptors();
}

void Renderer::uploadEnvironment(const Environment* env) {
    uint32_t id = env ? env->id : 0;
    if (id == envId_) return;
    vkDeviceWaitIdle(device_);   // (rare: a scene's sky image changed)
    destroyImage(envSky_);
    destroyImage(envRadiance_);
    if (env) {
        envSky_ = createFloatImage({&env->sky});
        std::vector<const Environment::Level*> levels;
        for (const Environment::Level& l : env->radiance) levels.push_back(&l);
        envRadiance_ = createFloatImage(levels);
        for (int k = 0; k < 9; ++k)
            for (int c = 0; c < 3; ++c) envSh_[k][c] = env->sh[k][c];
    } else {
        Environment::Level black;
        black.width = black.height = 1;
        black.rgb = {0.f, 0.f, 0.f};
        envSky_ = createFloatImage({&black});
        envRadiance_ = createFloatImage({&black});
    }
    envId_ = id;
    writeEnvironmentDescriptors();
}

void Renderer::destroyEnvironmentResources() {
    destroyImage(envSky_);
    destroyImage(envRadiance_);
    vkDestroySampler(device_, envSampler_, nullptr);
}
