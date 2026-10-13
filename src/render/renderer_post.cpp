// Post-processing: the HDR image, bloom down and up its mip chain, and the tonemapping composite.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/renderer_internal.h"

using namespace render_detail;

VkPipeline Renderer::createPostPipeline(const char* fragShader, VkFormat format, bool additive, VkPipelineLayout layout) {
    VkShaderModule vs = loadShader("post.vert.spv"), fs = loadShader(fragShader);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (additive) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout ? layout : postLayout_;
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &pipeline));
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return pipeline;
}

void Renderer::createPostImages() {
    hdr_ = createImage(hdrFormat_,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    uint32_t w = std::max(extent_.width / 2, 1u), h = std::max(extent_.height / 2, 1u);
    bloomMips_ = 1;
    while (bloomMips_ < kMaxBloomMips && std::min(w >> bloomMips_, h >> bloomMips_) >= 4) ++bloomMips_;
    bloom_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, w, h, VK_SAMPLE_COUNT_1_BIT, bloomMips_);
    for (uint32_t m = 0; m < bloomMips_; ++m) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = bloom_.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = hdrFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &bloomViews_[m]));
        bloomSizes_[m] = {std::max(w >> m, 1u), std::max(h >> m, 1u)};
    }
    // Point the post passes' descriptor sets at the new images
    auto write = [&](VkDescriptorSet set, VkImageView a, VkImageView b) {
        VkDescriptorImageInfo infos[2] = {{postSampler_, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                          {postSampler_, b, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w2[2];
        for (uint32_t i = 0; i < 2; ++i) {
            w2[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w2[i].dstSet = set;
            w2[i].dstBinding = i;
            w2[i].descriptorCount = 1;
            w2[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w2[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 2, w2, 0, nullptr);
    };
    write(prefilterSet_, hdr_.view, hdr_.view);
    for (uint32_t m = 1; m < bloomMips_; ++m) write(downSets_[m], bloomViews_[m - 1], bloomViews_[m - 1]);
    for (uint32_t m = 0; m + 1 < bloomMips_; ++m) write(upSets_[m], bloomViews_[m + 1], bloomViews_[m + 1]);
    write(compositeSet_, hdr_.view, bloomViews_[0]);

    // Ambient occlusion: half size, single channel; read by the composite (binding 2), made from the depth
    aoSize_ = {w, h};
    ao_ = createImage(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, w, h, VK_SAMPLE_COUNT_1_BIT, 1);
    VkDescriptorImageInfo aoInfo{postSampler_, ao_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wa.dstSet = compositeSet_;
    wa.dstBinding = 2;
    wa.descriptorCount = 1;
    wa.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wa.pImageInfo = &aoInfo;
    vkUpdateDescriptorSets(device_, 1, &wa, 0, nullptr);
    if (aoSupported_) {
        VkDescriptorImageInfo depthInfo{shadowSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        wa.dstSet = aoSet_;
        wa.dstBinding = 0;
        wa.pImageInfo = &depthInfo;
        vkUpdateDescriptorSets(device_, 1, &wa, 0, nullptr);
    }
    if (aa_ == Antialiasing::Temporal) createTaaImages(taa_);
    // Reflections' picture of the last frame, and the capture's inputs (the depth, this frame's picture)
    prevColor_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    prevColorFresh_ = true;
    if (aoSupported_) {
        VkDescriptorImageInfo infos[2] = {{shadowSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                          {postSampler_, hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w2[2];
        for (uint32_t i = 0; i < 2; ++i) {
            w2[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w2[i].dstSet = ssrSet_;
            w2[i].dstBinding = i;
            w2[i].descriptorCount = 1;
            w2[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w2[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 2, w2, 0, nullptr);
    }
    writeSsrBinding();
    // The UI layer (HDR output: the UI drawn apart, blended in by the composite): the composite's binding 3 (the HDR
    // image when there's none: something valid to bind)
    uiLayerFresh_ = uiLayered_;
    if (uiLayered_)
        uiLayer_ = createImage(sdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    {
        VkDescriptorImageInfo layer{postSampler_, uiLayer_.image ? uiLayer_.view : hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w3{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w3.dstSet = compositeSet_;
        w3.dstBinding = 3;
        w3.descriptorCount = 1;
        w3.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w3.pImageInfo = &layer;
        vkUpdateDescriptorSets(device_, 1, &w3, 0, nullptr);
    }
}

// The scene's view of the last frame's picture (set 0, binding 18), in every frame's set
void Renderer::writeSsrBinding() {
    if (!prevColor_.image) return;
    for (VkDescriptorSet set : descSets_) {
        if (!set) continue;   // (not made yet: createResources writes it once they are)
        VkDescriptorImageInfo info{postSampler_, prevColor_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 18;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
}

void Renderer::destroyPostImages() {
    if (prevColor_.image) destroyImage(prevColor_);
    if (uiLayer_.image) destroyImage(uiLayer_);
    destroyTaaImages(taa_);
    for (ViewImage& v : viewImages_) destroyTaaImages(v.taa);   // (sized like the targets: remade when next drawn)
    for (uint32_t m = 0; m < bloomMips_; ++m) vkDestroyImageView(device_, bloomViews_[m], nullptr);
    bloomMips_ = 0;
    if (bloom_.image) destroyImage(bloom_);
    if (ao_.image) destroyImage(ao_);
    if (hdr_.image) destroyImage(hdr_);
}

void Renderer::recordPost(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, int view) {
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    struct Push {
        float a[4], b[4], c[4], d[4];
    };
    mipBarrier(cb, hdr_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
               VK_ACCESS_SHADER_READ_BIT);

    // A full-screen pass into `view` (w x h), reading `set`
    auto pass = [&](VkImageView view, VkExtent2D size, VkPipeline pipeline, VkDescriptorSet set, const Push& push, bool load) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = target;
        att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, size};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp{0.f, 0.f, (float)size.width, (float)size.height, 0.f, 1.f};
        VkRect2D sc{{0, 0}, size};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
    };

    bool bloom = info.bloomStrength > 0.f && bloomMips_ > 0;
    // Every bloom level starts as a render target (the old contents don't matter)
    mipBarrier(cb, bloom_.image, 0, bloomMips_, VK_IMAGE_LAYOUT_UNDEFINED, bloom ? target : readable,
               colorStage | fragStage, 0, bloom ? colorStage : fragStage,
               bloom ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
    if (bloom) {
        // Down: the HDR image (bright parts only) into level 0, then each level into the next smaller one
        for (uint32_t m = 0; m < bloomMips_; ++m) {
            VkExtent2D src = m == 0 ? extent_ : bloomSizes_[m - 1];
            Push p{{1.f / (float)src.width, 1.f / (float)src.height, m == 0 ? 1.f : 0.f, info.bloomThreshold}, {}, {}, {}};
            pass(bloomViews_[m], bloomSizes_[m], bloomDownPipeline_, m == 0 ? prefilterSet_ : downSets_[m], p, false);
            mipBarrier(cb, bloom_.image, m, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       fragStage, VK_ACCESS_SHADER_READ_BIT);
        }
        // Up: each level blurred onto the next larger one (added to what's there)
        for (uint32_t m = bloomMips_ - 1; m-- > 0;) {
            mipBarrier(cb, bloom_.image, m, 1, readable, target, fragStage, VK_ACCESS_SHADER_READ_BIT, colorStage,
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkExtent2D src = bloomSizes_[m + 1];
            Push p{{1.f / (float)src.width, 1.f / (float)src.height, 1.f, 0.f}, {}, {}, {}};
            pass(bloomViews_[m], bloomSizes_[m], bloomUpPipeline_, upSets_[m], p, true);
            mipBarrier(cb, bloom_.image, m, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       fragStage, VK_ACCESS_SHADER_READ_BIT);
        }
    }

    // Ambient occlusion from the depth (or nothing: the composite then ignores it)
    float vx = std::clamp(info.viewportX, 0.f, (float)extent_.width);
    float vy = std::clamp(info.viewportY, 0.f, (float)extent_.height);
    float vw = std::min(info.viewportW, (float)extent_.width - vx);
    float vh = std::min(info.viewportH, (float)extent_.height - vy);
    bool ao = aoSupported_ && info.aoStrength > 0.f && info.items && vw >= 1.f && vh >= 1.f;
    mipBarrier(cb, ao_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, ao ? target : readable, colorStage | fragStage, 0,
               ao ? colorStage : fragStage, ao ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
    if (ao) {
        float tanY = std::tan(info.fovY * 0.5f), tanX = tanY * (vw / vh);
        Push p{{tanX, tanY, info.zNear, info.zFar}, {info.aoRadius, info.aoStrength, vx, vy}, {vw, vh, 0.f, 0.f}, {}};
        pass(ao_.view, aoSize_, aoPipeline_, aoSet_, p, false);
        mipBarrier(cb, ao_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                   VK_ACCESS_SHADER_READ_BIT);
    }

    // Composite: into the scene's viewport on the swapchain image, the rest getting the sky colour (under the UI); or
    // into an extra view's own image (sampled by the last frame's UI until now)
    VkImage outImage = view >= 0 ? viewImages_[view].image.image : swapImages_[imageIndex];
    VkExtent2D outExtent = view >= 0 ? VkExtent2D{viewImages_[view].width, viewImages_[view].height} : extent_;
    imageBarrier(cb, outImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, target,
                 colorStage | (view >= 0 ? fragStage : 0), 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = view >= 0 ? viewImages_[view].image.view : swapViews_[imageIndex];
    att.imageLayout = target;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{info.sky.x, info.sky.y, info.sky.z, 1.f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, outExtent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    // (with the UI in its own layer: over the whole picture, where the UI is blended in too)
    const bool layered = view < 0 && uiLayered_;
    if ((info.items && vw >= 1.f && vh >= 1.f) || layered) {
        VkViewport vp{vx, vy, vw, vh, 0.f, 1.f};
        VkRect2D sc{{(int32_t)vx, (int32_t)vy}, {(uint32_t)vw, (uint32_t)vh}};
        if (layered) {
            vp = {0.f, 0.f, (float)extent_.width, (float)extent_.height, 0.f, 1.f};
            sc = {{0, 0}, extent_};
        }
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, view >= 0 ? compositeViewPipeline_ : compositePipeline_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &compositeSet_, 0, nullptr);
        float output = view >= 0 ? 0.f : output_ == display::Output::Hdr10 ? 1.f : output_ == display::Output::ScRgb ? 2.f : 0.f;
        Push p{{info.exposure, bloom ? info.bloomStrength : 0.f, ao ? 1.f : 0.f, 0.f},
               {kOutlineSrgb.x, kOutlineSrgb.y, kOutlineSrgb.z, layered ? 1.f : 0.f},
               {(float)info.colorFilter, output, hdrPeak_, hdrPaperWhite_}, {vx, vy, vw, vh}};
        vkCmdPushConstants(cb, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &p);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);

    // Reflections next frame: this picture and its distances (the main view's only)
    if (view < 0 && info.reflections && info.items && ssrCapturePipeline_ && vw >= 1.f && vh >= 1.f) {
        imageBarrier(cb, prevColor_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target, fragStage,
                     VK_ACCESS_SHADER_READ_BIT, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        Push p{{info.zNear, info.zFar, 0.f, 0.f}, {}, {}, {}};
        pass(prevColor_.view, extent_, ssrCapturePipeline_, ssrSet_, p, false);
        imageBarrier(cb, prevColor_.image, VK_IMAGE_ASPECT_COLOR_BIT, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage, VK_ACCESS_SHADER_READ_BIT);
    }
}
