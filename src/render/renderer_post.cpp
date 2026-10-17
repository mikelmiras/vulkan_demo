// Post-processing: the HDR image, bloom down and up its mip chain, and the tonemapping composite.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/camera_fx.h"
#include "render/renderer_internal.h"
#include "render/starburst.h"

using namespace render_detail;

VkPipeline Renderer::createPostPipeline(const char* fragShader, VkFormat format, bool additive, VkPipelineLayout layout) {
    return createPostPipeline(fragShader, format, additive, layout, VK_FORMAT_UNDEFINED);
}

// (format2: a second colour attachment, written alike)
VkPipeline Renderer::createPostPipeline(const char* fragShader, VkFormat format, bool additive, VkPipelineLayout layout,
                                        VkFormat format2) {
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
    const uint32_t colours = format2 != VK_FORMAT_UNDEFINED ? 2 : 1;
    VkPipelineColorBlendAttachmentState cbas[2] = {cba, cba};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = colours;
    cb.pAttachments = cbas;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat formats[2] = {format, format2};
    rci.colorAttachmentCount = colours;
    rci.pColorAttachmentFormats = formats;
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
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // (HDR screenshots copy it out)
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
    write(starburstSet_, bloomViews_[0], bloomViews_[0]);   // (the starburst reads the bloom: binding 1)
    // The starburst's image: a quarter of the size each way (its streaks come from the half-size bloom)
    starburstSize_ = {std::max((w * 2 + 3) / 4, 1u), std::max((h * 2 + 3) / 4, 1u)};
    starburst_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             starburstSize_.width, starburstSize_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    starburstFresh_ = true;
    {
        VkDescriptorImageInfo info{postSampler_, starburst_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws.dstSet = compositeSet_;
        ws.dstBinding = 8;
        ws.descriptorCount = 1;
        ws.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &ws, 0, nullptr);
    }

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
    // Camera effects: the picture with depth of field and motion blur, copied back over it (fxSet_: the picture, the
    // depth, the motion vectors)
    fx_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                      extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    sssTemp_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    for (Image* tiles : {&fxTiles_, &fxTilesMax_})   // (r: the near field's circle, gb: the motion)
        *tiles = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, (extent_.width + 7) / 8, (extent_.height + 7) / 8, VK_SAMPLE_COUNT_1_BIT, 1);
    fxTilesFresh_ = true;
    // Light shafts: half size, read by the composite (binding 7), made from the depth (aoSet_'s)
    shafts_ = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, w, h, VK_SAMPLE_COUNT_1_BIT, 1);
    VkDescriptorImageInfo shaftsInfo{postSampler_, shafts_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    wa.dstBinding = 7;
    wa.pImageInfo = &shaftsInfo;
    vkUpdateDescriptorSets(device_, 1, &wa, 0, nullptr);
    if (aoSupported_) {
        VkDescriptorImageInfo depthInfo{shadowSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        wa.dstSet = aoSet_;
        wa.dstBinding = 0;
        wa.pImageInfo = &depthInfo;
        vkUpdateDescriptorSets(device_, 1, &wa, 0, nullptr);
    }
    // Motion vectors (the prepass writes them with temporal AA; the temporal pass reads them)
    velocity_ = createImage(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    velocityFresh_ = true;
    if (aoSupported_) {   // (the camera effects' set: the picture, the depth, the motion, the tiles)
        VkDescriptorImageInfo infos[5] = {{postSampler_, hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                          {shadowSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                          {postSampler_, velocity_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                          {nearestSampler_, fxTiles_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                          {nearestSampler_, fxTilesMax_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet w[5];
        for (uint32_t i = 0; i < 5; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = fxSet_;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 5, w, 0, nullptr);
    }
    createTraceImages();
    if (aa_ == Antialiasing::Temporal || upscaling()) createTaaImages(taa_);   // (temporal AA's, or the upscaler's)
    // Auto-exposure: the luminance (its mips: blits) and the exposure, now and last frame's; the composite reads
    // them (bindings 4, 5: something valid even without, the formats the shaders expect)
    if (lumSupported_) {
        lum_ = createImage(VK_FORMAT_R16_SFLOAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, kLumWidth, kLumHeight, VK_SAMPLE_COUNT_1_BIT, kLumMips);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = lum_.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R16_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &lumMip0_));
        for (Image& e : exposure_)
            e = createImage(VK_FORMAT_R32_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_SAMPLE_COUNT_1_BIT, 1);
        lumFresh_ = true;
        exposureHistory_ = false;
        auto bind = [&](VkDescriptorSet set, uint32_t binding, VkImageView view) {
            VkDescriptorImageInfo info{lumSampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &info;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        };
        bind(lumSet_, 0, hdr_.view);
        bind(adaptSet_, 0, lum_.view);
        bind(adaptSet_, 1, exposure_[1].view);
        bind(compositeSet_, 4, exposure_[0].view);
        bind(compositeSet_, 5, lum_.view);
    } else {   // (no auto-exposure here: the composite still binds something, never read)
        VkDescriptorImageInfo info{postSampler_, hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2];
        for (uint32_t i = 0; i < 2; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = compositeSet_;
            w[i].dstBinding = 4 + i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &info;
        }
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
    writeLensDirtBinding();
    // Reflections' picture of the last frame, and the capture's inputs (the depth, this frame's picture)
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, hdrFormat_, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        prevMips_ = (fp.optimalTilingFeatures & need) == need ? kPrevMips : 1;
    }
    prevColor_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, prevMips_);
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = prevColor_.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = hdrFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &prevMip0_));
    }
    prevColorFresh_ = true;
    // Water's refraction: the opaque picture and its distances, copied mid-pass (renderer.cpp)
    refraction_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, extent_.width, extent_.height, VK_SAMPLE_COUNT_1_BIT, 1);
    refractionFresh_ = true;
    writeRefractionBinding();
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

// A dirty lens: soft smudges of a few sizes, specks and a streak or two, a little coloured, on a faint film (the same
// every time: a fixed seed); lit only where the bloom is
void Renderer::createLensDirt() {
    Environment::Level level;
    level.width = level.height = 256;
    level.rgb.assign((size_t)256 * 256 * 3, 0.03f);
    uint32_t seed = 0x5eed1e55u;
    auto random = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (seed & 0xFFFFFF) / float(0x1000000);
    };
    auto splat = [&](float cx, float cy, float radius, float strength, Vec3 tint, float stretch) {
        int r = (int)std::ceil(radius * 3.f);
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                int px = (int)cx + x, py = (int)cy + y;
                if (px < 0 || py < 0 || px >= 256 || py >= 256) continue;
                float dx = (float)x / stretch, d2 = (dx * dx + (float)(y * y)) / (radius * radius);
                float w = strength * std::exp(-d2 * 2.f);
                float* p = &level.rgb[((size_t)py * 256 + px) * 3];
                p[0] += w * tint.x, p[1] += w * tint.y, p[2] += w * tint.z;
            }
    };
    for (int i = 0; i < 40; ++i)   // (smudges)
        splat(random() * 256.f, random() * 256.f, 6.f + random() * 22.f, 0.15f + random() * 0.35f,
              Vec3{0.9f + random() * 0.1f, 0.85f + random() * 0.15f, 0.75f + random() * 0.25f}, 1.f);
    for (int i = 0; i < 160; ++i)   // (specks)
        splat(random() * 256.f, random() * 256.f, 0.6f + random() * 1.4f, 0.4f + random() * 0.6f, Vec3{1, 1, 1}, 1.f);
    for (int i = 0; i < 3; ++i)   // (streaks: wiped)
        splat(random() * 256.f, random() * 256.f, 3.f + random() * 3.f, 0.25f, Vec3{0.9f, 0.9f, 1.f}, 8.f + random() * 8.f);
    for (float& v : level.rgb) v = std::min(v, 1.f);
    lensDirt_ = createFloatImage({&level});
    writeLensDirtBinding();
}

void Renderer::writeLensDirtBinding() {   // (until it's made, the HDR image stands in: never read with no dirt)
    if (!compositeSet_ || !hdr_.view) return;
    VkDescriptorImageInfo info{postSampler_, lensDirt_.image ? lensDirt_.view : hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = compositeSet_;
    w.dstBinding = 6;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

// Water's view of the opaque picture behind it (set 0, binding 26), in every frame's set
void Renderer::writeRefractionBinding() {
    if (!refraction_.image) return;
    for (VkDescriptorSet set : descSets_) {
        if (!set) continue;   // (not made yet: createResources writes it once they are)
        VkDescriptorImageInfo info{postSampler_, refraction_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 26;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
}

// The scene's view of the last frame's picture (set 0, binding 18), in every frame's set
void Renderer::writeSsrBinding() {
    if (!prevColor_.image) return;
    for (VkDescriptorSet set : descSets_) {
        if (!set) continue;   // (not made yet: createResources writes it once they are)
        VkDescriptorImageInfo info{lumSampler_, prevColor_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   // (every level)
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 18;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
}

float Renderer::compositeSharpness(int view) const {
    const bool upscaled = view < 0 && scaleNow_ < 0.999f;
    if (aa_ != Antialiasing::Temporal && !upscaled) return 0.f;   // (only what temporal AA or the upscaler softened)
    return sharpenOverride_ >= 0.f ? sharpenOverride_ : upscaled ? std::min(sharpness_ + 0.2f, 1.f) : sharpness_;
}

void Renderer::destroyPostImages() {
    if (refraction_.image) destroyImage(refraction_);
    if (shafts_.image) destroyImage(shafts_);
    if (fx_.image) destroyImage(fx_);
    if (fxTiles_.image) destroyImage(fxTiles_);
    if (fxTilesMax_.image) destroyImage(fxTilesMax_);
    if (sssTemp_.image) destroyImage(sssTemp_);
    if (velocity_.image) destroyImage(velocity_);
    for (Image* img : {&gbuffer_, &traced_[0], &traced_[1], &tracedLight_[0], &tracedLight_[1], &tracedDepth_, &aoLit_[0], &aoLit_[1],
                       &velocityMs_, &gbufferMs_, &sssSpecular_, &tracedLightBlur_, &clouds_[0], &clouds_[1]})
        if (img->image) destroyImage(*img);
    if (prevMip0_) vkDestroyImageView(device_, prevMip0_, nullptr);
    prevMip0_ = VK_NULL_HANDLE;
    if (prevColor_.image) destroyImage(prevColor_);
    if (lumMip0_) vkDestroyImageView(device_, lumMip0_, nullptr);
    lumMip0_ = VK_NULL_HANDLE;
    if (lum_.image) destroyImage(lum_);
    for (Image& e : exposure_)
        if (e.image) destroyImage(e);
    if (uiLayer_.image) destroyImage(uiLayer_);
    destroyTaaImages(taa_);
    for (ViewImage& v : viewImages_) destroyTaaImages(v.taa);   // (sized like the targets: remade when next drawn)
    for (uint32_t m = 0; m < bloomMips_; ++m) vkDestroyImageView(device_, bloomViews_[m], nullptr);
    bloomMips_ = 0;
    if (bloom_.image) destroyImage(bloom_);
    if (ao_.image) destroyImage(ao_);
    if (starburst_.image) destroyImage(starburst_);
    if (hdr_.image) destroyImage(hdr_);
}

// Subsurface diffusion (sss.glsl): after the lit pass (the picture a target, the depth readable, the specular it kept
// apart written), across into sssTemp_ and down back into the picture, which is left a target for temporal AA as it
// was found
void Renderer::recordSubsurface(VkCommandBuffer cb, const SceneRenderInfo& scene) {
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const float vx = std::clamp(scene.viewportX, 0.f, (float)extent_.width), vy = std::clamp(scene.viewportY, 0.f, (float)extent_.height);
    const float vw = std::max(std::min(scene.viewportW, (float)extent_.width - vx), 1.f);
    const float vh = std::max(std::min(scene.viewportH, (float)extent_.height - vy), 1.f);
    float push[24] = {1.f, 0.f, 0.02f, std::tan(scene.fovY * 0.5f), vx, vy, vw, vh, scene.zNear, scene.zFar, 1.f, 0.f};   // (2 cm)
    auto pass = [&](VkImageView out, VkDescriptorSet set) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = out;
        att.imageLayout = target;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // (every pixel written: the rest passed through)
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
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sssPipeline_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
    };
    mipBarrier(cb, hdr_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
               VK_ACCESS_SHADER_READ_BIT);
    mipBarrier(cb, sssSpecular_.image, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, fragStage, VK_ACCESS_SHADER_WRITE_BIT,
               fragStage, VK_ACCESS_SHADER_READ_BIT);
    mipBarrier(cb, sssTemp_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, target, fragStage, 0, colorStage,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    pass(sssTemp_.view, sssSets_[0]);   // (across)
    mipBarrier(cb, sssTemp_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
               VK_ACCESS_SHADER_READ_BIT);
    mipBarrier(cb, hdr_.image, 0, 1, readable, target, fragStage, VK_ACCESS_SHADER_READ_BIT, colorStage,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    push[0] = 0.f, push[1] = 1.f;
    pass(hdr_.view, sssSets_[1]);   // (down, back into the picture)
}

void Renderer::recordPost(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, int view) {
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    struct Push {
        float a[4], b[4], c[4], d[4], e[4];   // (e: the composite's tone mapper and grading)
        float f[4];                           // (the composite's auto-exposure and local tone mapping)
        float g[4];                           // (the composite's lens flare and the bloom levels' scale)
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

    // Camera effects (the main view's): depth of field and motion blur, from the picture into fx_, copied back
    // (what follows reads the picture as it was: bloom, the composite)
    const bool blurred = view < 0 && info.motionBlur > 0.f && velocityOn_;
    const bool focused = view < 0 && info.depthOfField > 0.f && aoSupported_;
    if (cameraFxPipeline_ && fxTilesPipeline_ && fxDilatePipeline_ && fx_.image && info.items && (blurred || focused) && quality_ != Quality::Low) {
        float fx = std::clamp(info.viewportX, 0.f, (float)extent_.width), fy = std::clamp(info.viewportY, 0.f, (float)extent_.height);
        float fw = std::min(info.viewportW, (float)extent_.width - fx), fh = std::min(info.viewportH, (float)extent_.height - fy);
        mipBarrier(cb, fx_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, target, fragStage | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                   colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        const float circle = focused ? std::clamp(info.depthOfField, 0.f, 1.f) * 0.012f * fh : 0.f;
        // (the sky's motion: where its directions were last frame, with the camera's turn)
        float sky[9];
        camerafx::skyMotion(info.viewProj, fxPrevViewProj_, sky);
        bool turned = false;
        for (int k = 0; k < 9; ++k) turned = turned || std::fabs(sky[k] / std::max(std::fabs(sky[8]), 1e-6f) - (k % 4 == 0 ? 1.f : 0.f)) > 1e-5f;
        Push p{{info.zNear, info.zFar, std::max(info.focusDistance, 0.1f), circle}, {fx, fy, fw, fh},
               {scaleNow_, blurred ? std::clamp(info.motionBlur, 0.f, 1.f) : 0.f, (float)(frameCounter_ % 64), 0.f},
               {sky[0], sky[1], sky[2], blurred && turned ? 1.f : 0.f}, {sky[3], sky[4], sky[5], 0.f}, {sky[6], sky[7], sky[8], 0.f}};
        // (the tiles first: each one's near circle and motion, then the largest round each)
        const VkExtent2D tileSize{(extent_.width + 7) / 8, (extent_.height + 7) / 8};
        for (Image* tiles : {&fxTiles_, &fxTilesMax_}) {
            mipBarrier(cb, tiles->image, 0, 1, readable, target, fragStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            pass(tiles->view, tileSize, tiles == &fxTiles_ ? fxTilesPipeline_ : fxDilatePipeline_, fxSet_, p, false);
            mipBarrier(cb, tiles->image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                       VK_ACCESS_SHADER_READ_BIT);
        }
        pass(fx_.view, extent_, cameraFxPipeline_, fxSet_, p, false);
        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        mipBarrier(cb, fx_.image, 0, 1, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorStage,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        mipBarrier(cb, hdr_.image, 0, 1, readable, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fragStage, VK_ACCESS_SHADER_READ_BIT,
                   transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {extent_.width, extent_.height, 1};
        vkCmdCopyImage(cb, fx_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);
        mipBarrier(cb, hdr_.image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readable, transfer, VK_ACCESS_TRANSFER_WRITE_BIT,
                   fragStage, VK_ACCESS_SHADER_READ_BIT);
    }

    bool bloom = info.bloomStrength > 0.f && bloomMips_ > 0;
    // Every bloom level starts as a render target (the old contents don't matter)
    mipBarrier(cb, bloom_.image, 0, bloomMips_, VK_IMAGE_LAYOUT_UNDEFINED, bloom ? target : readable,
               colorStage | fragStage, 0, bloom ? colorStage : fragStage,
               bloom ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
    if (bloom) {
        // Down: the HDR image (bright parts only) into level 0, then each level into the next smaller one
        for (uint32_t m = 0; m < bloomMips_; ++m) {
            VkExtent2D src = m == 0 ? extent_ : bloomSizes_[m - 1];
            Push p{{1.f / (float)src.width, 1.f / (float)src.height, m == 0 ? 1.f : 0.f, info.bloomThreshold}, {}, {}, {}, {}};
            pass(bloomViews_[m], bloomSizes_[m], bloomDownPipeline_, m == 0 ? prefilterSet_ : downSets_[m], p, false);
            mipBarrier(cb, bloom_.image, m, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       fragStage, VK_ACCESS_SHADER_READ_BIT);
        }
        // Up: each level blurred onto the next larger one (added to what's there)
        for (uint32_t m = bloomMips_ - 1; m-- > 0;) {
            mipBarrier(cb, bloom_.image, m, 1, readable, target, fragStage, VK_ACCESS_SHADER_READ_BIT, colorStage,
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkExtent2D src = bloomSizes_[m + 1];
            Push p{{1.f / (float)src.width, 1.f / (float)src.height, 1.f, 0.f}, {}, {}, {}, {}};
            pass(bloomViews_[m], bloomSizes_[m], bloomUpPipeline_, upSets_[m], p, true);
            mipBarrier(cb, bloom_.image, m, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       fragStage, VK_ACCESS_SHADER_READ_BIT);
        }
    }
    // The starburst from the finished bloom (the main view's), at a quarter of the size
    const bool starred = view < 0 && bloom && info.starburst > 0.f && starburstPipeline_ && starburst_.image;
    if (starred) {
        mipBarrier(cb, starburst_.image, 0, 1, readable, target, fragStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        Push p{{1.f / (float)starburstSize_.width, 1.f / (float)starburstSize_.height, 0.f, 0.f}, {}, {}, {}, {}, {},
               {0.f, 0.f, 0.f, starburst::pack(info.starburstBlades, info.starburst)}};
        pass(starburst_.view, starburstSize_, starburstPipeline_, starburstSet_, p, false);
        mipBarrier(cb, starburst_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                   VK_ACCESS_SHADER_READ_BIT);
    }

    // Ambient occlusion from the depth (or nothing: the composite then ignores it)
    float vx = std::clamp(info.viewportX, 0.f, (float)extent_.width);
    float vy = std::clamp(info.viewportY, 0.f, (float)extent_.height);
    float vw = std::min(info.viewportW, (float)extent_.width - vx);
    float vh = std::min(info.viewportH, (float)extent_.height - vy);
    bool ao = aoSupported_ && info.aoStrength > 0.f && quality_ != Quality::Low && info.items && vw >= 1.f && vh >= 1.f &&
              !(view < 0 && aoLitOn_);   // (the main view's, done before lighting: on the indirect light alone)
    mipBarrier(cb, ao_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, ao ? target : readable, colorStage | fragStage, 0,
               ao ? colorStage : fragStage, ao ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
    if (ao) {
        float tanY = std::tan(info.fovY * 0.5f), tanX = tanY * (vw / vh);
        Push p{{tanX, tanY, info.zNear, info.zFar}, {info.aoRadius, info.aoStrength, vx, vy}, {vw, vh, view >= 0 ? 1.f : scaleNow_, quality_ == Quality::High ? 12.f : 8.f}, {}, {}};
        pass(ao_.view, aoSize_, aoPipeline_, aoSet_, p, false);
        mipBarrier(cb, ao_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                   VK_ACCESS_SHADER_READ_BIT);
    }

    // Light shafts (volumetric fog makes its own; split-screen views theirs, in their own rectangle): the clear air's
    // light marched along each view ray through the sun's cascades and the lamps' light (shafts.glsl: the frame's
    // set, the view's slot)
    const bool shafted = shaftsPipeline_ && shaftsOn_ && info.items && vw >= 1.f && vh >= 1.f;
    mipBarrier(cb, shafts_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, shafted ? target : readable, colorStage | fragStage, 0,
               shafted ? colorStage : fragStage, shafted ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
    if (shafted) {
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = shafts_.view;
        att.imageLayout = target;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, aoSize_};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cb, &ri);
        VkViewport sv{0.f, 0.f, (float)aoSize_.width, (float)aoSize_.height, 0.f, 1.f};
        VkRect2D sc{{0, 0}, aoSize_};
        vkCmdSetViewport(cb, 0, 1, &sv);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shaftsPipeline_);
        const uint32_t slot = view < 0 ? 0 : (uint32_t)(1 + view) * (uint32_t)uboStride_;   // (the view's frame data)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        mipBarrier(cb, shafts_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                   VK_ACCESS_SHADER_READ_BIT);
    }

    // Auto-exposure and local tone mapping (the main view's): the view's log2 luminance, its mips down to the mean,
    // and the exposure eased towards it (then kept for next frame)
    const bool metering = view < 0 && lum_.image && info.items && vw >= 1.f && vh >= 1.f &&
                          (info.autoExposure || info.localToneMapping > 0.f);
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (metering) {
        mipBarrier(cb, lum_.image, 0, 1, readable, target, fragStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        Push lp{{vx, vy, vw, vh}, {1.f / (float)extent_.width, 1.f / (float)extent_.height, 0.f, 0.f}, {}, {}, {}, {}};
        pass(lumMip0_, {kLumWidth, kLumHeight}, lumPipeline_, lumSet_, lp, false);
        mipBarrier(cb, lum_.image, 0, 1, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorStage,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        for (uint32_t m = 1; m < kLumMips; ++m) {   // (each level the average of the one above: blits)
            mipBarrier(cb, lum_.image, m, 1, readable, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fragStage, 0, transfer,
                       VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            blit.srcOffsets[1] = {(int32_t)std::max(kLumWidth >> (m - 1), 1u), (int32_t)std::max(kLumHeight >> (m - 1), 1u), 1};
            blit.dstOffsets[1] = {(int32_t)std::max(kLumWidth >> m, 1u), (int32_t)std::max(kLumHeight >> m, 1u), 1};
            vkCmdBlitImage(cb, lum_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lum_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_LINEAR);
            mipBarrier(cb, lum_.image, m, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer,
                       VK_ACCESS_TRANSFER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        }
        mipBarrier(cb, lum_.image, 0, kLumMips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readable, transfer, VK_ACCESS_TRANSFER_READ_BIT,
                   fragStage, VK_ACCESS_SHADER_READ_BIT);
        if (info.autoExposure) {
            mipBarrier(cb, exposure_[0].image, 0, 1, readable, target, fragStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            Push ap{{std::clamp(info.frameSeconds, 0.f, 0.25f), info.adaptationSpeed, info.exposureMin, info.exposureMax},
                    {exposureHistory_ ? 0.f : 1.f, (float)(kLumMips - 1), 0.f, 0.f}, {}, {}, {}, {}};
            pass(exposure_[0].view, {1, 1}, adaptPipeline_, adaptSet_, ap, false);
            // (copied to the other: next frame starts from it)
            mipBarrier(cb, exposure_[0].image, 0, 1, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorStage,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
            mipBarrier(cb, exposure_[1].image, 0, 1, readable, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fragStage, 0, transfer,
                       VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.extent = {1, 1, 1};
            vkCmdCopyImage(cb, exposure_[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, exposure_[1].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            mipBarrier(cb, exposure_[1].image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, readable, transfer,
                       VK_ACCESS_TRANSFER_WRITE_BIT, fragStage, VK_ACCESS_SHADER_READ_BIT);
            mipBarrier(cb, exposure_[0].image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readable, transfer,
                       VK_ACCESS_TRANSFER_READ_BIT, fragStage, VK_ACCESS_SHADER_READ_BIT);
        }
    }
    if (view < 0) exposureHistory_ = metering && info.autoExposure;
    const bool autoExposed = lum_.image && info.autoExposure && (metering || view >= 0) && exposureHistory_;
    const float localTone = metering ? std::clamp(info.localToneMapping, 0.f, 1.f) : 0.f;

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
        Push p{{info.exposure, bloom ? info.bloomStrength : 0.f, ao ? 1.f : 0.f, shafted ? 1.f : 0.f},
               {kOutlineSrgb.x, kOutlineSrgb.y, kOutlineSrgb.z, layered ? 1.f : 0.f},
               {(float)info.colorFilter, output, hdrPeak_, hdrPaperWhite_}, {vx, vy, vw, vh},
               {(float)info.tonemapper, info.contrast, info.saturation, info.vignette},
               {autoExposed ? 1.f : 0.f, localTone, (float)(kLumMips - 1), lensDirt_.image ? std::clamp(info.lensDirt, 0.f, 1.f) : 0.f},
               // (the glow's scale: the levels past the six it was tuned with widen it, not brighten it)
               {bloom ? std::clamp(info.lensFlare, 0.f, 1.f) : 0.f, bloomMips_ > 6 ? 6.f / (float)bloomMips_ * 1.15f : 1.f,
                compositeSharpness(view), starred ? starburst::pack(info.starburstBlades, info.starburst) : 0.f}};
        vkCmdPushConstants(cb, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &p);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);

    // Reflections and GI next frame: this picture and its distances (the main view's only), then its levels
    if (view < 0 && captureOn(info) && info.items && ssrCapturePipeline_ && vw >= 1.f && vh >= 1.f) {
        mipBarrier(cb, prevColor_.image, 0, 1, readable, target, fragStage, VK_ACCESS_SHADER_READ_BIT, colorStage,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        Push p{{info.zNear, info.zFar, 0.f, 0.f}, {vx, vy, scaleNow_, 0.f}, {}, {}, {}};
        pass(prevMip0_, extent_, ssrCapturePipeline_, ssrSet_, p, false);
        if (prevMips_ > 1) {
            mipBarrier(cb, prevColor_.image, 0, 1, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorStage,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
            for (uint32_t m = 1; m < prevMips_; ++m) {
                mipBarrier(cb, prevColor_.image, m, 1, readable, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fragStage, 0, transfer,
                           VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                blit.srcOffsets[1] = {(int32_t)std::max(extent_.width >> (m - 1), 1u), (int32_t)std::max(extent_.height >> (m - 1), 1u), 1};
                blit.dstOffsets[1] = {(int32_t)std::max(extent_.width >> m, 1u), (int32_t)std::max(extent_.height >> m, 1u), 1};
                vkCmdBlitImage(cb, prevColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, prevColor_.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                mipBarrier(cb, prevColor_.image, m, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           transfer, VK_ACCESS_TRANSFER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
            }
            mipBarrier(cb, prevColor_.image, 0, prevMips_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readable, transfer,
                       VK_ACCESS_TRANSFER_READ_BIT, fragStage | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        } else {
            mipBarrier(cb, prevColor_.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       fragStage | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        recordNear(cb);   // (its near pyramid: the reflections' skipping)
    }
}
