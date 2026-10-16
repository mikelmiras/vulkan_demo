// Particles on the GPU (ParticleEmitter::gpu; shaders/particles.comp): each GPU emitter gets a ring of slots in one
// state buffer (kept by its entity while its size stays; laid out again, cleared, when the set of them changes), the
// CPU tells each how many to spawn this frame, and a compute pass before the scene moves them all and writes the
// live ones into this frame's particle buffer after the CPU's, drawn by one indirect, additive draw.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/renderer_internal.h"

using namespace render_detail;

namespace {
struct GpuParticleState {
    float posAge[4], velLife[4], misc[4];
};
struct GpuEmitterData {
    float pos[4], up[4], a[4], b[4], size[4], colour0[4], colour1[4];
    uint32_t range[4];
};
constexpr uint32_t kMaxGpuEmitters = 64, kMaxGpuParticles = 1u << 17;
}  // namespace

void Renderer::createGpuParticles() {
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 4;
    dslci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &gpuParticleSetLayout_));
    VkDescriptorSetLayout layouts[2] = {setLayout_, gpuParticleSetLayout_};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &gpuParticleLayout_));
    VkShaderModule cs = loadShader("particles.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = gpuParticleLayout_;
    VK_CHECK(vkCreateComputePipelines(device_, pipelineCache_, 1, &ci, nullptr, &gpuParticlePipeline_));
    vkDestroyShaderModule(device_, cs, nullptr);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * MAX_FRAMES};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &gpuParticlePool_));
    std::vector<VkDescriptorSetLayout> setLayouts(MAX_FRAMES, gpuParticleSetLayout_);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = gpuParticlePool_;
    dsai.descriptorSetCount = MAX_FRAMES;
    dsai.pSetLayouts = setLayouts.data();
    VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, gpuParticleSets_));
    for (int i = 0; i < MAX_FRAMES; ++i) {
        gpuEmitterBuffers_[i] = createBuffer(sizeof(GpuEmitterData) * kMaxGpuEmitters, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        gpuIndirect_[i] = createDeviceBuffer(sizeof(VkDrawIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
}

void Renderer::destroyGpuParticles() {
    for (int i = 0; i < MAX_FRAMES; ++i) {
        destroyBuffer(gpuEmitterBuffers_[i]);
        destroyBuffer(gpuIndirect_[i]);
    }
    if (gpuParticleState_.buffer) destroyBuffer(gpuParticleState_);
    vkDestroyPipeline(device_, gpuParticlePipeline_, nullptr);
    vkDestroyPipelineLayout(device_, gpuParticleLayout_, nullptr);
    vkDestroyDescriptorPool(device_, gpuParticlePool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, gpuParticleSetLayout_, nullptr);
}

// This frame's GPU emitters: their slots (laid out again when the set of them changes), how many each spawns
void Renderer::prepareGpuParticles(const SceneRenderInfo& info) {
    gpuParticleSlots_ = 0;
    gpuEmitterCount_ = 0;
    if (!info.gpuEmitters || info.gpuEmitters->empty() || !gpuParticlePipeline_) return;
    std::vector<std::pair<uint32_t, uint32_t>> layout;   // (key, slots)
    for (const GpuEmitterInfo& e : *info.gpuEmitters) {
        if (layout.size() >= kMaxGpuEmitters) break;
        layout.push_back({e.key, (uint32_t)std::clamp(e.maxParticles, 1, 65536)});
    }
    if (layout != gpuLayout_) {   // (laid out afresh: every slot dead)
        gpuLayout_ = layout;
        gpuRings_.clear();
        uint32_t offset = 0;
        for (auto [key, slots] : layout) {
            if (offset + slots > kMaxGpuParticles) slots = kMaxGpuParticles > offset ? kMaxGpuParticles - offset : 0;
            gpuRings_.push_back({key, offset, slots, 0, 0.f});
            offset += slots;
        }
        gpuTotalSlots_ = offset;
        gpuClearState_ = true;
        if (!gpuParticleState_.buffer || gpuParticleState_.size < std::max<size_t>(offset, 1) * sizeof(GpuParticleState)) {
            vkDeviceWaitIdle(device_);   // (a bigger buffer: rare, when emitters are added)
            if (gpuParticleState_.buffer) destroyBuffer(gpuParticleState_);
            gpuParticleState_ = createDeviceBuffer(std::max<size_t>(offset, 1) * sizeof(GpuParticleState),
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            gpuParticleState_.size = std::max<size_t>(offset, 1) * sizeof(GpuParticleState);
        }
    }
    const float dt = std::clamp(info.frameSeconds, 0.f, 0.1f);
    auto* out = static_cast<GpuEmitterData*>(gpuEmitterBuffers_[frame_].mapped);
    for (size_t k = 0; k < gpuRings_.size(); ++k) {
        GpuRing& r = gpuRings_[k];
        const GpuEmitterInfo& e = (*info.gpuEmitters)[k];
        if (r.slots == 0) continue;
        r.pending += info.gpuEmitting ? std::max(e.rate, 0.f) * dt : 0.f;
        uint32_t spawn = std::min((uint32_t)r.pending, r.slots);
        r.pending -= (float)spawn;
        GpuEmitterData d{};
        d.pos[0] = e.position.x, d.pos[1] = e.position.y, d.pos[2] = e.position.z, d.pos[3] = (float)spawn;
        d.up[0] = e.up.x, d.up[1] = e.up.y, d.up[2] = e.up.z, d.up[3] = std::cos(std::clamp(e.spread, 0.f, 180.f) * DEG2RAD);
        d.a[0] = e.lifetime, d.a[1] = e.lifetimeRandom, d.a[2] = e.speed, d.a[3] = e.speedRandom;
        d.b[0] = e.radius, d.b[1] = e.gravity, d.b[2] = e.drag, d.b[3] = std::max(e.stretch, 0.f);
        d.size[0] = e.startSize, d.size[1] = e.endSize, d.size[2] = e.lit ? 1.f : 0.f;
        d.colour0[0] = e.startColor.x, d.colour0[1] = e.startColor.y, d.colour0[2] = e.startColor.z, d.colour0[3] = e.startAlpha;
        d.colour1[0] = e.endColor.x, d.colour1[1] = e.endColor.y, d.colour1[2] = e.endColor.z, d.colour1[3] = e.endAlpha;
        d.range[0] = r.offset, d.range[1] = r.slots, d.range[2] = r.head, d.range[3] = (uint32_t)(frameCounter_ * 2654435761u + k);
        out[gpuEmitterCount_++] = d;
        r.head = (r.head + spawn) % r.slots;
    }
    gpuParticleSlots_ = gpuTotalSlots_;
}

// Before the scene pass: the draw count reset to none past the CPU's particles, the pass, its results to the draw
void Renderer::recordGpuParticles(VkCommandBuffer cb) {
    if (!gpuParticleSlots_) return;
    const VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    auto barrier = [&](VkPipelineStageFlags from, VkAccessFlags fromAccess, VkPipelineStageFlags to, VkAccessFlags toAccess) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = fromAccess;
        m.dstAccessMask = toAccess;
        vkCmdPipelineBarrier(cb, from, to, 0, 1, &m, 0, nullptr, 0, nullptr);
    };
    // (last frame's pass and draw used these: done before they're written again)
    barrier(compute | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT, transfer | compute,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (gpuClearState_) {   // (laid out afresh: every slot dead, its life 0)
        vkCmdFillBuffer(cb, gpuParticleState_.buffer, 0, gpuParticleState_.size, 0);
        gpuClearState_ = false;
    }
    VkDrawIndirectCommand cmd{6, 0, 0, particleAlpha_ + particleAdditive_};
    vkCmdUpdateBuffer(cb, gpuIndirect_[frame_].buffer, 0, sizeof(cmd), &cmd);
    barrier(transfer, VK_ACCESS_TRANSFER_WRITE_BIT, compute, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    VkDescriptorBufferInfo infos[4] = {{gpuParticleState_.buffer, 0, VK_WHOLE_SIZE},
                                       {gpuEmitterBuffers_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {particles_[frame_].buffer, 0, VK_WHOLE_SIZE},
                                       {gpuIndirect_[frame_].buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[4];
    for (uint32_t i = 0; i < 4; ++i) {   // (this frame's set: its fence has passed)
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = gpuParticleSets_[frame_];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
    const uint32_t slot = 0;   // (the main view's frame data)
    VkDescriptorSet sets[2] = {descSets_[frame_], gpuParticleSets_[frame_]};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpuParticleLayout_, 0, 2, sets, 1, &slot);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gpuParticlePipeline_);
    struct {
        uint32_t emitters, total;
        float dt;
        uint32_t flags;
    } push{gpuEmitterCount_, gpuParticleSlots_, gpuDt_, gpuSplash_ ? 1u : 0u};
    vkCmdPushConstants(cb, gpuParticleLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, (gpuParticleSlots_ + 63) / 64, 1, 1);
    barrier(compute, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    timestamp(cb, "GPU particles");
}
