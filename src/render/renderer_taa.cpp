// Anti-aliasing: switching between MSAA, temporal and none, and the temporal pass (taa.frag): each frame is drawn with
// a sub-pixel jitter, then blended with the frames before it, found where they were by reprojecting the depth.
#include "render/renderer.h"

#include "render/renderer_internal.h"

#include <algorithm>
#include <cstring>

using namespace render_detail;

bool Renderer::supports(Antialiasing mode) const {
    if (mode == Antialiasing::Msaa) return maxSamples_ != VK_SAMPLE_COUNT_1_BIT;
    if (mode == Antialiasing::Temporal) return aoSupported_;   // (it reads the depth)
    return true;
}

void Renderer::setAntialiasing(Antialiasing mode) {
    if (!supports(mode)) mode = Antialiasing::Off;
    if (mode == aa_) return;
    vkDeviceWaitIdle(device_);
    aa_ = mode;
    VkSampleCountFlagBits samples = mode == Antialiasing::Msaa ? maxSamples_ : VK_SAMPLE_COUNT_1_BIT;
    if (samples != samples_) {   // the targets and the pipelines drawing into them
        samples_ = samples;
        destroySampledPipelines();
        createSampledPipelines();
    }
    recreateSwapchain();   // (and the post images: the temporal ones come and go with the mode)
}

void Renderer::createTaaImages(TaaState& t) {
    for (Image& img : t.img)
        img = createImage(hdrFormat_,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    t.fresh = true;   // (made readable by the first pass: this may run before there's a command pool)
    // Set k writes img[k]: it reads this frame's image, img[1 - k] (the history) and the depth
    for (int k = 0; k < 2; ++k) {
        VkDescriptorImageInfo infos[3] = {
            {postSampler_, hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {postSampler_, t.img[1 - k].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {shadowSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[3];
        for (uint32_t i = 0; i < 3; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = t.sets[k];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }
    t.history = false;
}

void Renderer::destroyTaaImages(TaaState& t) {
    for (Image& img : t.img)
        if (img.image) destroyImage(img);
    t.history = false;
}

// A different sub-pixel offset every frame (Halton 2, 3: eight of them), the same for every view
void Renderer::jitterView(const SceneRenderInfo& info, FrameUBO& u, TaaState& t, const SceneRenderInfo* display) {
    auto halton = [](uint32_t i, uint32_t b) {
        float f = 1.f, r = 0.f;
        for (; i > 0; i /= b) {
            f /= (float)b;
            r += f * (float)(i % b);
        }
        return r;
    };
    float vw = std::max(info.viewportW, 1.f), vh = std::max(info.viewportH, 1.f);   // (the picture drawn: its pixels)
    Mat4 jitter;
    t.jitterPx[0] = halton(jitterIndex_, 2) - 0.5f;
    t.jitterPx[1] = halton(jitterIndex_, 3) - 0.5f;
    jitter.m[12] = t.jitterPx[0] * 2.f / vw;
    jitter.m[13] = t.jitterPx[1] * 2.f / vh;
    u.viewProj = jitter * info.viewProj;
    // From this frame's clip space to last frame's: where each pixel was. Both without the jitter, so a still view
    // finds its history exactly at the same pixel (no blur creeping in from resampling)
    t.reproject = t.prevViewProj * inverse(info.viewProj);
    const SceneRenderInfo& kept = display ? *display : info;   // (the history's: full size, whatever the scale)
    float viewport[4] = {kept.viewportX, kept.viewportY, kept.viewportW, kept.viewportH};
    if (std::memcmp(viewport, t.viewport, sizeof(viewport)) != 0) t.history = false;   // (moved or resized)
    std::memcpy(t.viewport, viewport, sizeof(viewport));
    t.prevViewProj = info.viewProj;
}

void Renderer::recordTaa(VkCommandBuffer cb, const SceneRenderInfo& info, TaaState& t, float scale) {
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    Image& out = t.img[t.index];
    if (t.fresh) {   // both readable to start with (the history isn't used until there's one)
        for (Image& img : t.img)
            imageBarrier(cb, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, readable,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, fragStage, VK_ACCESS_SHADER_READ_BIT);
        t.fresh = false;
    }
    imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, target, readable, colorStage,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage, VK_ACCESS_SHADER_READ_BIT);
    imageBarrier(cb, out.image, VK_IMAGE_ASPECT_COLOR_BIT, readable, target, fragStage | transfer, 0, colorStage,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = out.view;
    att.imageLayout = target;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp{0.f, 0.f, (float)extent_.width, (float)extent_.height, 0.f, 1.f};
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, aa_ == Antialiasing::Temporal ? taaPipeline_ : taaUpscalePipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, taaLayout_, 0, 1, &t.sets[t.index], 0, nullptr);
    struct Push {
        Mat4 reproject;
        float viewport[4];   // the scene's rectangle in the image (pixels)
        float params[4];     // x = how much history (0: none), y, z = 1 / image size, w = the render scale
        float extra[4];      // xy = this frame's jitter (drawn pixels), z = sharpening (upscaled)
    } p;
    p.reproject = t.reproject;
    p.viewport[0] = info.viewportX;
    p.viewport[1] = info.viewportY;
    p.viewport[2] = std::max(info.viewportW, 1.f);
    p.viewport[3] = std::max(info.viewportH, 1.f);
    const bool temporal = aa_ == Antialiasing::Temporal;
    p.params[0] = t.history && temporal ? 0.9f : 0.f;   // (upscaling alone: no history)
    p.params[1] = 1.f / (float)extent_.width;
    p.params[2] = 1.f / (float)extent_.height;
    p.params[3] = std::clamp(scale, 0.5f, 1.f);
    p.extra[0] = temporal ? t.jitterPx[0] : 0.f;
    p.extra[1] = temporal ? t.jitterPx[1] : 0.f;
    p.extra[2] = scale < 0.999f ? (temporal ? 0.25f : 0.5f) : 0.f;
    p.extra[3] = 0.f;
    vkCmdPushConstants(cb, taaLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);

    // The result back into the HDR image (the passes after read that), and kept as next frame's history
    imageBarrier(cb, out.image, VK_IMAGE_ASPECT_COLOR_BIT, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorStage,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
    imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, readable, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fragStage,
                 VK_ACCESS_SHADER_READ_BIT, transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(cb, out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &copy);
    imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target, transfer,
                 VK_ACCESS_TRANSFER_WRITE_BIT, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readable, transfer, 0,
                 fragStage, VK_ACCESS_SHADER_READ_BIT);
    t.index = 1 - t.index;
    t.history = true;
}
