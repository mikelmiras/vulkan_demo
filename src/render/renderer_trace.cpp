// Reflections and global illumination traced at half size (renderer.h: traced_): without MSAA, the depth prepass
// writes each pixel's surface (normal, roughness) beside its motion; ssr_trace.frag and gi_trace.frag then trace
// one pixel of each 2 x 2 block, a different one each frame, through the last frame's picture, keeping a history
// of the others; scene.frag brings them up to full size (depth-aware) instead of tracing per pixel.
#include "render/renderer.h"

#include <algorithm>

#include "render/renderer_internal.h"

using namespace render_detail;

void Renderer::createTraceImages() {
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const uint32_t hw = std::max((extent_.width + 1) / 2, 1u), hh = std::max((extent_.height + 1) / 2, 1u);
    gbuffer_ = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height,
                           VK_SAMPLE_COUNT_1_BIT, 1);
    for (int k = 0; k < 2; ++k) {
        traced_[k] = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, hw, hh, VK_SAMPLE_COUNT_1_BIT, 1);
        tracedLight_[k] = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, hw, hh, VK_SAMPLE_COUNT_1_BIT, 1);
    }
    tracedDepth_ = createImage(VK_FORMAT_R32_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, hw, hh, VK_SAMPLE_COUNT_1_BIT, 1);
    traceHistory_ = false;
    writeTraceBindings();
}

// Every frame's set: frame i's results are traced_[i] (what it draws with) and traced_[1 - i] its history: frames
// take turns (MAX_FRAMES is 2)
void Renderer::writeTraceBindings() {
    static_assert(MAX_FRAMES == 2, "the traces' history takes turns with the frames");
    if (!gbuffer_.image) return;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (!descSets_[i]) continue;   // (not made yet: createResources writes them once they are)
        const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::vector<std::pair<uint32_t, VkDescriptorImageInfo>> images = {
            {30, {nearestSampler_, gbuffer_.view, readable}},
            {31, {postSampler_, traced_[i].view, readable}},
            {32, {postSampler_, tracedLight_[i].view, readable}},
            {33, {postSampler_, traced_[1 - i].view, readable}},
            {34, {postSampler_, tracedLight_[1 - i].view, readable}},
            {35, {nearestSampler_, tracedDepth_.view, readable}},
            {36, {nearestSampler_, velocity_.view, readable}}};
        if (aoSupported_)
            images.push_back({29, {shadowSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}});
        std::vector<VkWriteDescriptorSet> w(images.size(), {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        for (size_t k = 0; k < images.size(); ++k) {
            w[k].dstSet = descSets_[i];
            w[k].dstBinding = images[k].first;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &images[k].second;
        }
        vkUpdateDescriptorSets(device_, (uint32_t)w.size(), w.data(), 0, nullptr);
    }
}

// After the prepass (its depth, surface and motion written), before the lit pass: the two half-size passes
void Renderer::recordTrace(VkCommandBuffer cb, const SceneRenderInfo& scene) {
    if (!traceOn_) return;
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 fragStage, VK_ACCESS_SHADER_READ_BIT);
    const VkExtent2D half{std::max((extent_.width + 1) / 2, 1u), std::max((extent_.height + 1) / 2, 1u)};
    auto pass = [&](VkPipeline pipeline, std::initializer_list<const Image*> outs) {
        VkRenderingAttachmentInfo atts[2] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        uint32_t n = 0;
        for (const Image* img : outs) {
            mipBarrier(cb, img->image, 0, 1, readable, target, fragStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            atts[n].imageView = img->view;
            atts[n].imageLayout = target;
            atts[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ++n;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, half};
        ri.layerCount = 1;
        ri.colorAttachmentCount = n;
        ri.pColorAttachments = atts;
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp{0.f, 0.f, (float)half.width, (float)half.height, 0.f, 1.f};
        VkRect2D sc{{0, 0}, half};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const uint32_t slot = 0;   // (the main view's frame data)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        for (const Image* img : outs)
            mipBarrier(cb, img->image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                       VK_ACCESS_SHADER_READ_BIT);
    };
    pass(ssrTracePipeline_, {&traced_[frame_], &tracedDepth_});
    if (ssgiOn(scene)) pass(giTracePipeline_, {&tracedLight_[frame_]});
    imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, fragStage, 0, depthStages,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    timestamp(cb, "Half-size traces");
}
