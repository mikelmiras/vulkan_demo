// Extra views (SceneRenderInfo::views: the editor's game view, split-screen players): each drawn whole into the
// shared targets, one after another before the main view, and composited into an image of its own that Dear ImGui
// shows wherever the host puts it.
#include "render/renderer.h"

#include <algorithm>

#include "imgui_impl_vulkan.h"
#include "render/renderer_internal.h"

using namespace render_detail;

ImTextureID Renderer::viewTexture(int view, uint32_t width, uint32_t height) {
    if (view < 0 || view >= kMaxViews || width == 0 || height == 0) return (ImTextureID)0;
    width = std::min(width, 8192u);
    height = std::min(height, 8192u);
    ViewImage& v = viewImages_[view];
    if (v.image.image && v.width == width && v.height == height) return (ImTextureID)v.ui;
    if (v.image.image) {
        vkDeviceWaitIdle(device_);   // (a frame in flight may still be drawing it, or showing it)
        destroyViewImage(v);
    }
    v.image = createImage(swapFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, width, height, VK_SAMPLE_COUNT_1_BIT, 1);
    v.width = width;
    v.height = height;
    v.ui = ImGui_ImplVulkan_AddTexture(postSampler_, v.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    v.fresh = true;
    return (ImTextureID)v.ui;
}

void Renderer::destroyViewImage(ViewImage& v) {
    if (v.ui) ImGui_ImplVulkan_RemoveTexture(v.ui);
    if (v.image.image) destroyImage(v.image);
    destroyTaaImages(v.taa);
    VkDescriptorSet sets[2] = {v.taa.sets[0], v.taa.sets[1]};   // (allocated once: kept)
    v = ViewImage{};
    v.taa.sets[0] = sets[0];
    v.taa.sets[1] = sets[1];
}

void Renderer::recordView(VkCommandBuffer cb, int index, const SceneRenderInfo& view) {
    ViewImage& target = viewImages_[index];
    if (!target.image.image) return;   // (nobody asked for its texture)
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags depthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    // In the targets' top-left corner, as big as its image (or the targets) allow
    float w = std::min({view.viewportW, (float)target.width, (float)extent_.width});
    float h = std::min({view.viewportH, (float)target.height, (float)extent_.height});
    if (w < 1.f || h < 1.f) return;
    SceneRenderInfo info = view;
    info.viewportX = info.viewportY = 0.f;
    info.viewportW = w;
    info.viewportH = h;

    // The targets: what was in them (the view before, last frame's passes) waited for and dropped
    imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 colorStage | fragStage | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (colorMsaa_.image)
        imageBarrier(cb, colorMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 depthStages | fragStage | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    Vec3 sky = srgbToLinear(info.sky);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // (all of it: bloom reads a little past the view's edge)
    color.clearValue.color = {{sky.x, sky.y, sky.z, 1.f}};
    if (colorMsaa_.image) {
        color.imageView = colorMsaa_.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = hdr_.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = hdr_.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = aoSupported_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    vkCmdBeginRendering(cb, &ri);

    const uint32_t slot = (uint32_t)(uboStride_ * (1 + index));   // (its frame data)
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
    VkViewport viewport{0.f, 0.f, w, h, 0.f, 1.f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    auto draw = [&](const Batch& b) {
        if (b.texture != bound) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
            bound = b.texture;
        }
        drawBatch(cb, b);
        stats_.drawCalls++;
    };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline_);
    for (const Batch& b : viewBatches_[index]) draw(b);
    drawPlants(cb, info);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (!viewTransparent_[index].empty()) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline_);
        bound = VK_NULL_HANDLE;
        for (const Batch& b : viewTransparent_[index]) draw(b);
    }
    if (particleAlpha_) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAlphaPipeline_);
        vkCmdDraw(cb, 6, particleAlpha_, 0, 0);
    }
    if (particleAdditive_) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAdditivePipeline_);
        vkCmdDraw(cb, 6, particleAdditive_, 0, particleAlpha_);
    }
    vkCmdEndRendering(cb);
    if (aoSupported_)   // (the depth, for its ambient occlusion)
        imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     fragStage, VK_ACCESS_SHADER_READ_BIT);

    if (aa_ == Antialiasing::Temporal && aoSupported_) {   // its own history (made now the first time)
        if (!target.taa.img[0].image) createTaaImages(target.taa);
        recordTaa(cb, info, target.taa);
    }
    // AO, bloom and the composite, into its image; then readable by Dear ImGui
    recordPost(cb, 0, info, index);
    imageBarrier(cb, target.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                 VK_ACCESS_SHADER_READ_BIT);
    target.fresh = false;
}
