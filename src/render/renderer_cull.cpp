// GPU culling: the main pass's solid objects are written with their bounds, all of them; a compute shader
// (cull.comp) tests each against the view frustum and counts the visible ones into each batch's indirect draw.
#include "render/renderer.h"

#include <cstdlib>
#include <cstring>

#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

struct CullBounds {
    float centre[4];   // w: the batch (the bits of a uint)
    float extent[4];   // w > 0: never culled
};

}  // namespace

void Renderer::createCulling() {
    if (const char* v = std::getenv("VKWORLD_GPU_CULLING"); v && std::strcmp(v, "0") == 0) gpuCulling_ = false;
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 3;
    dslci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &cullSetLayout_));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 6 * 16 + 4};   // the frustum's planes, the count
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &cullSetLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &cullLayout_));
    VkShaderModule cs = loadShader("cull.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main",
                nullptr};
    ci.layout = cullLayout_;
    VK_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &cullPipeline_));
    vkDestroyShaderModule(device_, cs, nullptr);

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_FRAMES};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &cullPool_));
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = cullPool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &cullSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &cullSets_[i]));
        // Small to start with (they grow with the scene)
        cullBounds_[i] = createBuffer(64 * sizeof(CullBounds), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        cullBounds_[i].size = 64 * sizeof(CullBounds);
        cullCommands_[i] = createBuffer(64 * sizeof(VkDrawIndexedIndirectCommand),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        cullCommands_[i].size = 64 * sizeof(VkDrawIndexedIndirectCommand);
    }
}

void Renderer::destroyCulling() {
    for (int i = 0; i < MAX_FRAMES; ++i)
        for (Buffer* b : {&cullBounds_[i], &cullCommands_[i], &instanceIndex_[i]})
            if (b->buffer) destroyBuffer(*b);
    vkDestroyDescriptorPool(device_, cullPool_, nullptr);
    vkDestroyPipeline(device_, cullPipeline_, nullptr);
    vkDestroyPipelineLayout(device_, cullLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, cullSetLayout_, nullptr);
}

void Renderer::writeCullSet() {
    if (!instanceIndex_[frame_].buffer) return;
    VkDescriptorBufferInfo infos[3] = {{cullBounds_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {cullCommands_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {instanceIndex_[frame_].buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = cullSets_[frame_];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
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
    grow(cullCommands_[frame_], batches, sizeof(VkDrawIndexedIndirectCommand),
         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (changed) writeCullSet();
}

// The main pass's batches (the first `visible.size()` instances, in order): each object's bounds and batch, and a
// draw per batch with no instances yet (the compute pass counts them in)
void Renderer::prepareGpuCulling(const std::vector<DrawItem>& items, const std::vector<uint32_t>& visible) {
    ensureCullCapacity(visible.size(), sceneBatches_.size());
    auto* bounds = static_cast<CullBounds*>(cullBounds_[frame_].mapped);
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(cullCommands_[frame_].mapped);
    for (size_t bi = 0; bi < sceneBatches_.size(); ++bi) {
        const Batch& b = sceneBatches_[bi];
        const Mesh& mesh = meshes_->get(b.mesh);
        commands[bi] = {mesh.indexCount, 0, mesh.firstIndex, mesh.vertexOffset, b.first};
        uint32_t batch = (uint32_t)bi;
        float batchBits;
        std::memcpy(&batchBits, &batch, 4);
        for (uint32_t k = b.first; k < b.first + b.count; ++k) {
            const DrawItem& o = items[visible[k]];
            const Mat4& m = o.model;
            Vec3 c = (mesh.boundsMin + mesh.boundsMax) * 0.5f, e = (mesh.boundsMax - mesh.boundsMin) * 0.5f;
            Vec3 wc = transformPoint(m, c);   // (the box's world centre and half extents, as Frustum::visible)
            Vec3 we{std::fabs(m.m[0]) * e.x + std::fabs(m.m[4]) * e.y + std::fabs(m.m[8]) * e.z,
                    std::fabs(m.m[1]) * e.x + std::fabs(m.m[5]) * e.y + std::fabs(m.m[9]) * e.z,
                    std::fabs(m.m[2]) * e.x + std::fabs(m.m[6]) * e.y + std::fabs(m.m[10]) * e.z};
            bounds[k] = {{wc.x, wc.y, wc.z, batchBits}, {we.x, we.y, we.z, o.jointOffset >= 0 ? 1.f : 0.f}};
        }
    }
    cullCount_[frame_] = (uint32_t)visible.size();
    cullBatches_[frame_] = (uint32_t)sceneBatches_.size();
    gpuCulled_ = true;
}

void Renderer::recordCulling(VkCommandBuffer cb, const SceneRenderInfo& info) {
    if (!gpuCulled_ || cullCount_[frame_] == 0) return;
    Frustum f(info.viewProj);
    struct Push {
        float planes[6][4];
        uint32_t count;
    } p;
    std::memcpy(p.planes, f.p, sizeof(p.planes));
    p.count = cullCount_[frame_];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout_, 0, 1, &cullSets_[frame_], 0, nullptr);
    vkCmdPushConstants(cb, cullLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 6 * 16 + 4, &p);
    vkCmdDispatch(cb, (p.count + 63) / 64, 1, 1);
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
    for (uint32_t i = 0; i < cullBatches_[frame_]; ++i) {
        gpuVisible_ += commands[i].instanceCount;
        gpuTriangles_ += commands[i].indexCount / 3 * commands[i].instanceCount;
    }
}
