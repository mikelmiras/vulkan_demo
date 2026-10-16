// GPU culling: the main pass's solid objects are written with their bounds, all of them; a compute shader
// (cull.comp) tests each against the view frustum and, with occlusion culling, against the depth pyramid the frame
// before left (an object hidden then behind what drew there stays hidden: one frame late when things move apart),
// picks its level of detail, and counts the visible ones into their batch's indirect draws (three per batch, one
// per level).
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "assets/mesh_lod.h"
#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

struct CullBounds {
    float centre[4];    // w: the batch (the bits of a uint)
    float extent[4];    // w > 0: never culled
    uint32_t info[4];   // x: the instance, y: its mesh's levels of detail
};

struct CullParams {   // std140, as cull.comp's Params
    float planes[6][4];
    float camPos[4];
    uint32_t counts[4];
    float lodSizes[4];
    Mat4 prevViewProj;
    int32_t hiz[4];
};

constexpr uint32_t kLodSlots = 1 + kMaxLods;   // indirect draws per batch

}  // namespace

void Renderer::createCulling() {
    if (const char* v = std::getenv("VKWORLD_GPU_CULLING"); v && std::strcmp(v, "0") == 0) gpuCulling_ = false;
    if (const char* v = std::getenv("VKWORLD_OCCLUSION"); v && std::strcmp(v, "0") == 0) occlusion_ = false;
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 3; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 5;
    dslci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &cullSetLayout_));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &cullSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &cullLayout_));
    VkShaderModule cs = loadShader("cull.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main",
                nullptr};
    ci.layout = cullLayout_;
    VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &cullPipeline_));
    vkDestroyShaderModule(device_, cs, nullptr);

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_FRAMES},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &cullPool_));
    createHiz();
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = cullPool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &cullSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &cullSets_[i]));
        // Small to start with (they grow with the scene)
        cullBounds_[i] = createBuffer(64 * sizeof(CullBounds), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        cullBounds_[i].size = 64 * sizeof(CullBounds);
        cullCommands_[i] = createBuffer(64 * kLodSlots * sizeof(VkDrawIndexedIndirectCommand),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        cullCommands_[i].size = 64 * kLodSlots * sizeof(VkDrawIndexedIndirectCommand);
        cullParams_[i] = createBuffer(sizeof(CullParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    }
}

void Renderer::destroyCulling() {
    for (int i = 0; i < MAX_FRAMES; ++i)
        for (Buffer* b : {&cullBounds_[i], &cullCommands_[i], &instanceIndex_[i], &cullParams_[i]})
            if (b->buffer) destroyBuffer(*b);
    destroyHiz();
    vkDestroyDescriptorPool(device_, cullPool_, nullptr);
    vkDestroyPipeline(device_, cullPipeline_, nullptr);
    vkDestroyPipelineLayout(device_, cullLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, cullSetLayout_, nullptr);
}

void Renderer::writeCullSet() {
    if (!instanceIndex_[frame_].buffer) return;
    VkDescriptorBufferInfo infos[4] = {{cullBounds_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {cullCommands_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {instanceIndex_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {cullParams_[frame_].buffer, 0, sizeof(CullParams)}};
    VkDescriptorImageInfo pyramid{hizSampler_, hiz_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[5];
    for (uint32_t i = 0; i < 5; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = cullSets_[frame_];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                            : i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i < 4) w[i].pBufferInfo = &infos[i];
        else w[i].pImageInfo = &pyramid;
    }
    vkUpdateDescriptorSets(device_, 5, w, 0, nullptr);
}

// (this frame's fence has been waited on: its buffers are free to replace)
void Renderer::ensureCullCapacity(size_t objects, size_t batches) {
    bool changed = false;
    auto grow = [&](Buffer& b, size_t need, size_t stride, VkBufferUsageFlags usage) {
        if (b.size >= need * stride) return;
        size_t capacity = 64;
        while (capacity < need) capacity *= 2;
        destroyBuffer(b);
        b = createBuffer(capacity * stride, usage);
        b.size = capacity * stride;
        changed = true;
    };
    grow(cullBounds_[frame_], objects, sizeof(CullBounds), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    grow(cullCommands_[frame_], batches * kLodSlots, sizeof(VkDrawIndexedIndirectCommand),
         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (changed) writeCullSet();
}

// The main pass's batches (the first `visible.size()` index list entries, in order): each object's bounds,
// instance and batch, and three draws per batch with no instances yet (the compute pass counts them in): level 0
// draws from the batch's own entries, levels 1 and 2 from regions after the list
void Renderer::prepareGpuCulling(const std::vector<DrawItem>& items, const std::vector<uint32_t>& visible,
                                 const SceneRenderInfo& info) {
    ensureCullCapacity(visible.size(), sceneBatches_.size());
    auto* bounds = static_cast<CullBounds*>(cullBounds_[frame_].mapped);
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(cullCommands_[frame_].mapped);
    const uint32_t count = (uint32_t)visible.size();
    for (size_t bi = 0; bi < sceneBatches_.size(); ++bi) {
        const Batch& b = sceneBatches_[bi];
        const Mesh& mesh = meshes_->get(b.mesh);
        for (uint32_t k = 0; k < kLodSlots; ++k) {
            Batch level = b;
            level.lod = (int)k;
            MeshLod r = rangeOf(level);   // (levels the mesh hasn't: its own range, never used)
            uint32_t first = k == 0 ? b.first : listedInstances_ + (k - 1) * count + b.first;
            commands[bi * kLodSlots + k] = {r.indexCount, 0, r.firstIndex, r.vertexOffset, first};
        }
        uint32_t batch = (uint32_t)bi;
        float batchBits;
        std::memcpy(&batchBits, &batch, 4);
        const auto* index = static_cast<const uint32_t*>(instanceIndex_[frame_].mapped);
        for (uint32_t k = b.first; k < b.first + b.count; ++k) {
            const DrawItem& o = items[index[k]];
            const Mat4& m = o.model;
            Vec3 c = (mesh.boundsMin + mesh.boundsMax) * 0.5f, e = (mesh.boundsMax - mesh.boundsMin) * 0.5f;
            Vec3 wc = transformPoint(m, c);   // (the box's world centre and half extents, as Frustum::visible)
            Vec3 we{std::fabs(m.m[0]) * e.x + std::fabs(m.m[4]) * e.y + std::fabs(m.m[8]) * e.z,
                    std::fabs(m.m[1]) * e.x + std::fabs(m.m[5]) * e.y + std::fabs(m.m[9]) * e.z,
                    std::fabs(m.m[2]) * e.x + std::fabs(m.m[6]) * e.y + std::fabs(m.m[10]) * e.z};
            bool skinned = o.jointOffset >= 0;
            bounds[k] = {{wc.x, wc.y, wc.z, batchBits},
                         {we.x, we.y, we.z, skinned ? 1.f : 0.f},
                         {index[k], skinned ? 0u : (uint32_t)mesh.lods.size(), 0, 0}};
        }
    }
    // The pass's parameters
    CullParams p{};
    Frustum f(info.viewProj);
    std::memcpy(p.planes, f.p, sizeof(p.planes));
    p.camPos[0] = info.camPos.x;
    p.camPos[1] = info.camPos.y;
    p.camPos[2] = info.camPos.z;
    p.camPos[3] = std::tan(info.fovY * 0.5f);
    p.counts[0] = count;
    p.counts[1] = occlusion_ && hizValid_ ? 1u : 0u;
    p.lodSizes[0] = kLod1Size;
    p.lodSizes[1] = kLod2Size;
    p.prevViewProj = hizViewProj_;
    p.hiz[0] = (int32_t)hizWidth_;
    p.hiz[1] = (int32_t)hizHeight_;
    p.hiz[2] = (int32_t)hizMips_;
    std::memcpy(cullParams_[frame_].mapped, &p, sizeof(p));
    cullCount_[frame_] = count;
    cullBatches_[frame_] = (uint32_t)sceneBatches_.size();
    gpuCulled_ = true;
}

void Renderer::recordCulling(VkCommandBuffer cb, const SceneRenderInfo&) {
    if (!gpuCulled_ || cullCount_[frame_] == 0) return;
    // Earlier frames' draws read these buffers (the index list, the commands): their reads come before these
    // writes; the pyramid was written by the frame before
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout_, 0, 1, &cullSets_[frame_], 0, nullptr);
    vkCmdDispatch(cb, (cullCount_[frame_] + 63) / 64, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
}

void Renderer::readCullStats() {
    if (!cullBatches_[frame_]) return;   // (that frame wasn't culled on the GPU)
    const auto* commands = static_cast<const VkDrawIndexedIndirectCommand*>(cullCommands_[frame_].mapped);
    gpuVisible_ = gpuTriangles_ = 0;
    for (uint32_t i = 0; i < cullBatches_[frame_] * kLodSlots; ++i) {
        gpuVisible_ += commands[i].instanceCount;
        gpuTriangles_ += commands[i].indexCount / 3 * commands[i].instanceCount;
    }
}

// ---------------------------------------------------------------------------------------------
// The depth pyramid
// ---------------------------------------------------------------------------------------------
void Renderer::createHiz() {
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &hizSetLayout_));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &hizSetLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &hizLayout_));
    auto pipeline = [&](const char* file) {
        VkShaderModule cs = loadShader(file);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
                    "main", nullptr};
        ci.layout = hizLayout_;
        VkPipeline p;
        VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &p));
        vkDestroyShaderModule(device_, cs, nullptr);
        return p;
    };
    hizDepthPipeline_ = pipeline("hiz_depth.comp.spv");
    hizDepthMsPipeline_ = pipeline("hiz_depth_ms.comp.spv");
    hizReducePipeline_ = pipeline("hiz_reduce.comp.spv");
    nearPipeline_ = pipeline("near_z.comp.spv");   // (the reflections' pyramid: the same layout)
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 16;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &hizPool_));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &hizSampler_));
    // A 1 x 1 stand-in until there's a viewport (cull.comp's binding must be valid; it's never tested against)
    SceneRenderInfo none;
    none.viewportW = none.viewportH = 1.f;
    ensureHiz(none);
}

void Renderer::destroyHiz() {
    if (!hizPool_) return;
    for (VkImageView v : hizLevels_) vkDestroyImageView(device_, v, nullptr);
    hizLevels_.clear();
    if (hiz_.image) destroyImage(hiz_);
    vkDestroyDescriptorPool(device_, hizPool_, nullptr);
    for (VkPipeline p : {hizDepthPipeline_, hizDepthMsPipeline_, hizReducePipeline_, nearPipeline_}) vkDestroyPipeline(device_, p, nullptr);
    for (VkImageView v : nearLevels_) vkDestroyImageView(device_, v, nullptr);
    nearLevels_.clear();
    if (near_.image) destroyImage(near_);
    if (nearPool_) vkDestroyDescriptorPool(device_, nearPool_, nullptr);
    nearPool_ = VK_NULL_HANDLE;
    nearSize_ = {0, 0};
    vkDestroyPipelineLayout(device_, hizLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, hizSetLayout_, nullptr);
    vkDestroySampler(device_, hizSampler_, nullptr);
    hizPool_ = VK_NULL_HANDLE;
}

void Renderer::ensureHiz(const SceneRenderInfo& info) {
    uint32_t w = (uint32_t)std::max(info.viewportW, 1.f), h = (uint32_t)std::max(info.viewportH, 1.f);
    if (hiz_.image && w == hizWidth_ && h == hizHeight_ && hizDepthView_ == depthSampleView_) return;
    if (hiz_.image) vkDeviceWaitIdle(device_);   // (frames in flight read it: rare, a resize)
    for (VkImageView v : hizLevels_) vkDestroyImageView(device_, v, nullptr);
    hizLevels_.clear();
    if (hiz_.image) destroyImage(hiz_);
    if (!hizSets_.empty()) vkFreeDescriptorSets(device_, hizPool_, (uint32_t)hizSets_.size(), hizSets_.data());
    hizSets_.clear();
    hizWidth_ = w;
    hizHeight_ = h;
    hizMips_ = 1;
    while (((std::max(w, h)) >> hizMips_) > 0 && hizMips_ < 15) ++hizMips_;
    hiz_ = createImage(VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, w, h, VK_SAMPLE_COUNT_1_BIT, hizMips_);
    for (uint32_t m = 0; m < hizMips_; ++m) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = hiz_.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R32_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        VkImageView v;
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &v));
        hizLevels_.push_back(v);
    }
    // Level k's set: level k - 1 (or the depth) to read, level k to write
    hizDepthView_ = depthSampleView_;
    std::vector<VkDescriptorSetLayout> layouts(hizMips_, hizSetLayout_);
    hizSets_.resize(hizMips_);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = hizPool_;
    dsai.descriptorSetCount = hizMips_;
    dsai.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, hizSets_.data()));
    for (uint32_t m = 0; m < hizMips_; ++m) {
        VkDescriptorImageInfo src{hizSampler_, m == 0 ? depthSampleView_ : hizLevels_[m - 1],
                                  m == 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst{VK_NULL_HANDLE, hizLevels_[m], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        wr[0].dstSet = wr[1].dstSet = hizSets_[m];
        wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &src;
        wr[1].dstBinding = 1;
        wr[1].descriptorCount = 1;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[1].pImageInfo = &dst;
        // (no depth view to read without ambient occlusion support: level 0's set is left half-written, unused)
        if (m == 0 && !depthSampleView_) vkUpdateDescriptorSets(device_, 1, &wr[1], 0, nullptr);
        else vkUpdateDescriptorSets(device_, 2, wr, 0, nullptr);
    }
    // GENERAL from the start, all "far" (nothing hidden)
    VkCommandBuffer cb = beginOneTimeCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = hiz_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMips_, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue far{{1.f, 1.f, 1.f, 1.f}};
    vkCmdClearColorImage(cb, hiz_.image, VK_IMAGE_LAYOUT_GENERAL, &far, 1, &b.subresourceRange);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    endOneTimeCommands(cb);
    hizValid_ = false;
    int keep = (int)frame_;   // (both frames' culling sets point at the new pyramid: the GPU is idle)
    for (int f = 0; f < MAX_FRAMES; ++f) {
        frame_ = (uint32_t)f;
        if (cullSets_[f]) writeCullSet();
    }
    frame_ = (uint32_t)keep;
}

void Renderer::ensureNear() {
    if (!nearPipeline_ || !prevColor_.image || !prevMip0_) return;
    const VkExtent2D size{std::max((extent_.width + 1) / 2, 1u), std::max((extent_.height + 1) / 2, 1u)};
    if (near_.image && size.width == nearSize_.width && size.height == nearSize_.height && nearSource_ == prevMip0_) return;
    if (near_.image) vkDeviceWaitIdle(device_);   // (frames in flight read it: a resize)
    for (VkImageView v : nearLevels_) vkDestroyImageView(device_, v, nullptr);
    nearLevels_.clear();
    if (near_.image) destroyImage(near_);
    if (!nearPool_) {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 3;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &nearPool_));
        std::vector<VkDescriptorSetLayout> layouts(3, hizSetLayout_);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = nearPool_;
        dsai.descriptorSetCount = 3;
        dsai.pSetLayouts = layouts.data();
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, nearSets_));
    }
    nearSize_ = size;
    nearSource_ = prevMip0_;   // (remade with the picture: an anti-aliasing change keeps its size, not its views)
    near_ = createImage(VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, size.width, size.height, VK_SAMPLE_COUNT_1_BIT, 3);
    for (uint32_t m = 0; m < 3; ++m) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = near_.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R32_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        VkImageView v;
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &v));
        nearLevels_.push_back(v);
    }
    // Level k's set: the picture's first level (its alpha) or level k - 1 to read, level k to write
    for (uint32_t m = 0; m < 3; ++m) {
        VkDescriptorImageInfo src{hizSampler_, m == 0 ? prevMip0_ : nearLevels_[m - 1],
                                  m == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo dst{VK_NULL_HANDLE, nearLevels_[m], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        wr[0].dstSet = wr[1].dstSet = nearSets_[m];
        wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &src;
        wr[1].dstBinding = 1;
        wr[1].descriptorCount = 1;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[1].pImageInfo = &dst;
        vkUpdateDescriptorSets(device_, 2, wr, 0, nullptr);
    }
    // GENERAL from the start, all 0 (everything near: the trace steps finely until a picture's been captured)
    VkCommandBuffer cb = beginOneTimeCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = near_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue zero{{0.f, 0.f, 0.f, 0.f}};
    vkCmdClearColorImage(cb, near_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    endOneTimeCommands(cb);
    // Every frame's set: binding 39 (all three levels, sampled by level)
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = near_.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1};
    if (near_.view) vkDestroyImageView(device_, near_.view, nullptr);
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &near_.view));
    for (VkDescriptorSet set : descSets_) {
        if (!set) continue;
        VkDescriptorImageInfo info{hizSampler_, near_.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 39;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
}

// After last frame's picture is captured (readable): its near pyramid, for this frame's reflections next
void Renderer::recordNear(VkCommandBuffer cb) {
    if (!near_.image) return;
    auto barrier = [&](VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = near_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1};
        vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT);   // (this frame's traces read it)
    struct Push {
        int32_t a[4], b[4];
    } p{};
    for (uint32_t m = 0; m < 3; ++m) {
        uint32_t w = std::max(nearSize_.width >> m, 1u), h = std::max(nearSize_.height >> m, 1u);
        p = {{m == 0 ? 1 : 0, 0, (int32_t)w, (int32_t)h}, {}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, nearPipeline_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hizLayout_, 0, 1, &nearSets_[m], 0, nullptr);
        vkCmdPushConstants(cb, hizLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                m < 2 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
}

// After the main pass (its depth readable): the pyramid, for the next frame's culling
void Renderer::recordHiz(VkCommandBuffer cb, const SceneRenderInfo& info) {
    if (!occlusion_ || !gpuCulled_ || !depthSampleView_ || hizDepthView_ != depthSampleView_) {
        hizValid_ = false;
        return;
    }
    if (hizWidth_ != (uint32_t)std::max(info.viewportW, 1.f) || hizHeight_ != (uint32_t)std::max(info.viewportH, 1.f)) {
        hizValid_ = false;   // (made for another size: ensureHiz remakes it before the next frame)
        return;
    }
    auto imageBarrierAll = [&](VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = hiz_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, hizMips_, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    };
    imageBarrierAll(VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);   // (this frame's culling read it)
    struct Push {
        int32_t a[4], b[4];
    } p{};
    float vx = std::clamp(info.viewportX, 0.f, (float)extent_.width), vy = std::clamp(info.viewportY, 0.f, (float)extent_.height);
    for (uint32_t m = 0; m < hizMips_; ++m) {
        uint32_t w = std::max(hizWidth_ >> m, 1u), h = std::max(hizHeight_ >> m, 1u);
        VkPipeline pipeline = m > 0 ? hizReducePipeline_ : samples_ != VK_SAMPLE_COUNT_1_BIT ? hizDepthMsPipeline_ : hizDepthPipeline_;
        p = {{m == 0 ? (int32_t)vx : 0, m == 0 ? (int32_t)vy : 0, (int32_t)w, (int32_t)h}, {(int32_t)samples_, 0, 0, 0}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hizLayout_, 0, 1, &hizSets_[m], 0, nullptr);
        vkCmdPushConstants(cb, hizLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        imageBarrierAll(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);   // (the next level reads this one)
    }
    hizViewProj_ = info.viewProj;
    hizValid_ = true;
}
