// Dynamic diffuse global illumination: a grid of light probes around the camera (kGiX x kGiY x kGiZ, a lattice
// RenderSettings::giSpacing apart). Each frame the nearest probes that aren't drawn yet (else the ones drawn longest
// ago, nearer first) are drawn: six 32 x 32 faces of the lit scene into an atlas, nearby objects only and at their
// coarsest level of detail. gi_project.comp turns each into irradiance (nine spherical harmonics) in its slot of
// giProbes_; scene.frag blends the eight around a point (common.glsl's giOrigin / giDims / giCell). The faces are lit
// with the probes too, so light bounces again with every redraw. Slots are the cells' coordinates modulo the grid's
// size: moving the camera reassigns only the slots that left the grid, the rest keep their light.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

struct GiDispatch {   // std140, as gi_project.comp's Dispatch
    float probe[4][4];      // xyz = position, w = slot
    float blend[4][4];      // x = how much of the new picture to take
    float face[6][3][4];    // per face: direction through its centre, per unit of NDC x, per unit of NDC y
    int32_t info[4];        // x = face size
};

const Vec3 kGiForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const Vec3 kGiUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};

int wrap(int v, int n) { return ((v % n) + n) % n; }

}  // namespace

void Renderer::createGi() {
    const size_t bytes = (size_t)kGiProbes * 10 * 16;
    giProbes_ = createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(giProbes_.mapped, 0, bytes);   // (nothing made yet: w = 0)
    for (int i = 0; i < MAX_FRAMES; ++i) {
        giDispatch_[i] = createBuffer(sizeof(GiDispatch), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        // scene.frag's view of the probes (set 0, binding 17)
        VkDescriptorBufferInfo bi{giProbes_.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descSets_[i];
        w.dstBinding = 17;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    // The projection pass
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 3;
    dslci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &giSetLayout_));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &giSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &giLayout_));
    VkShaderModule cs = loadShader("gi_project.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr};
    ci.layout = giLayout_;
    VK_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &giPipeline_));
    vkDestroyShaderModule(device_, cs, nullptr);
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &giPool_));
    // The atlas: a row of six faces per probe drawn in a frame
    giAtlas_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           6 * kGiFace, kGiPerFrame * kGiFace, VK_SAMPLE_COUNT_1_BIT, 1);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = giPool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &giSetLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &giSets_[i]));
        VkDescriptorImageInfo atlas{postSampler_, giAtlas_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo probes{giProbes_.buffer, 0, VK_WHOLE_SIZE}, params{giDispatch_[i].buffer, 0, sizeof(GiDispatch)};
        VkWriteDescriptorSet w[3];
        for (uint32_t k = 0; k < 3; ++k) {
            w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[k].dstSet = giSets_[i];
            w[k].dstBinding = k;
            w[k].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &atlas;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &probes;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[2].pBufferInfo = &params;
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }
    giSamples_ = VK_SAMPLE_COUNT_1_BIT;   // (the multisampled targets: made by planGi to match the pipelines)
}

void Renderer::destroyGi() {
    if (giAtlasMsaa_.image) destroyImage(giAtlasMsaa_);
    if (giDepth_.image) destroyImage(giDepth_);
    if (giAtlas_.image) destroyImage(giAtlas_);
    for (Buffer& b : giDispatch_)
        if (b.buffer) destroyBuffer(b);
    if (giProbes_.buffer) destroyBuffer(giProbes_);
    if (giPool_) vkDestroyDescriptorPool(device_, giPool_, nullptr);
    if (giPipeline_) vkDestroyPipeline(device_, giPipeline_, nullptr);
    if (giLayout_) vkDestroyPipelineLayout(device_, giLayout_, nullptr);
    if (giSetLayout_) vkDestroyDescriptorSetLayout(device_, giSetLayout_, nullptr);
}

void Renderer::planGi(const SceneRenderInfo& info) {
    giCount_ = 0;
    giOn_ = info.gi && info.items && giPipeline_;
    std::memset(giGrid_, 0, sizeof(giGrid_));
    if (!giOn_) return;
    if (!giDepth_.image || giSamples_ != samples_) {   // (the scene pipelines' sample count: MSAA on or off)
        vkDeviceWaitIdle(device_);
        if (giAtlasMsaa_.image) destroyImage(giAtlasMsaa_);
        if (giDepth_.image) destroyImage(giDepth_);
        giSamples_ = samples_;
        uint32_t w = 6 * kGiFace, h = kGiPerFrame * kGiFace;
        if (samples_ != VK_SAMPLE_COUNT_1_BIT)
            giAtlasMsaa_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT, w, h, samples_, 1);
        giDepth_ = createImage(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, w, h, samples_, 1);
    }
    ++giFrame_;
    const float s = std::max(info.giSpacing, 0.25f);
    const int dims[3] = {kGiX, kGiY, kGiZ};
    int first[3];
    const float cam[3] = {info.camPos.x, info.camPos.y, info.camPos.z};
    for (int a = 0; a < 3; ++a) first[a] = (int)std::floor(cam[a] / s) - dims[a] / 2 + 1;
    for (int a = 0; a < 3; ++a) {
        giGrid_[0][a] = (float)first[a] * s;
        giGrid_[1][a] = (float)dims[a];
        giGrid_[2][a] = (float)first[a];
    }
    giGrid_[0][3] = s;
    giGrid_[1][3] = info.giIntensity;

    // The cells the grid covers now: slots that held others start again
    struct Candidate {
        int slot;
        float score;   // lower first
    };
    std::vector<Candidate> order;
    order.reserve(kGiProbes);
    for (int x = 0; x < kGiX; ++x)
        for (int y = 0; y < kGiY; ++y)
            for (int z = 0; z < kGiZ; ++z) {
                int cell[3] = {first[0] + x, first[1] + y, first[2] + z};
                int slot = (wrap(cell[0], kGiX) * kGiY + wrap(cell[1], kGiY)) * kGiZ + wrap(cell[2], kGiZ);
                GiSlot& g = giSlots_[slot];
                if (g.cell[0] != cell[0] || g.cell[1] != cell[1] || g.cell[2] != cell[2] || !g.drawn) {
                    std::memcpy(g.cell, cell, sizeof(cell));
                    g.made = false;
                }
                float dx = (float)cell[0] * s - cam[0], dy = (float)cell[1] * s - cam[1], dz = (float)cell[2] * s - cam[2];
                float near = 1.f + std::sqrt(dx * dx + dy * dy + dz * dz) / s;
                float score = g.made ? near / (1.f + (float)(giFrame_ - g.drawn)) : -1e6f / near;
                order.push_back({slot, score});
            }
    std::partial_sort(order.begin(), order.begin() + kGiPerFrame, order.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

    // The probes to draw: their faces' frame data, and the projection's parameters
    giFaces_.assign((size_t)kGiPerFrame * 6 * sizeof(FrameUBO), 0);
    FrameUBO* faces = reinterpret_cast<FrameUBO*>(giFaces_.data());
    GiDispatch d{};
    d.info[0] = kGiFace;
    for (int n = 0; n < kGiPerFrame; ++n) {
        GiSlot& g = giSlots_[order[(size_t)n].slot];
        Vec3 pos{(float)g.cell[0] * s, (float)g.cell[1] * s, (float)g.cell[2] * s};
        giDrawSlot_[n] = order[(size_t)n].slot;
        giDrawPos_[n] = pos;
        d.probe[n][0] = pos.x, d.probe[n][1] = pos.y, d.probe[n][2] = pos.z, d.probe[n][3] = (float)giDrawSlot_[n];
        d.blend[n][0] = g.made ? 0.5f : 1.f;   // (redrawn: eased towards the new light; new: as it is)
        g.made = true;
        g.drawn = giFrame_;
        for (int f = 0; f < 6; ++f) {
            SceneRenderInfo fi = info;
            fi.views = nullptr;
            fi.camPos = pos;
            fi.camForward = kGiForward[f];
            fi.fovY = 90.f * DEG2RAD;
            fi.aspect = 1.f;
            fi.zNear = 0.05f;
            fi.zFar = std::max(s * 24.f, 50.f);
            fi.viewProj = perspective(fi.fovY, 1.f, fi.zNear, fi.zFar, true) * lookAt(pos, pos + kGiForward[f], kGiUp[f]);
            fi.viewportX = fi.viewportY = 0.f;
            fi.viewportW = fi.viewportH = (float)kGiFace;
            fillFrameUBO(fi, faces[n * 6 + f], false);   // (finished by finishGi)
            // Directions through the face for the projection: points of one depth are linear in NDC x and y
            Mat4 inv = inverse(fi.viewProj);
            auto unproject = [&](float x, float y) {
                float p[4];
                for (int r = 0; r < 4; ++r) p[r] = inv.m[0 * 4 + r] * x + inv.m[1 * 4 + r] * y + inv.m[2 * 4 + r] * 0.5f + inv.m[3 * 4 + r];
                return Vec3{p[0] / p[3], p[1] / p[3], p[2] / p[3]} - pos;
            };
            Vec3 c = unproject(0.f, 0.f), dx = unproject(1.f, 0.f) - c, dy = unproject(0.f, 1.f) - c;
            const Vec3 basis[3] = {c, dx, dy};
            for (int k = 0; k < 3; ++k) {
                d.face[f][k][0] = basis[k].x;
                d.face[f][k][1] = basis[k].y;
                d.face[f][k][2] = basis[k].z;
            }
        }
        ++giCount_;
    }
    std::memcpy(giDispatch_[frame_].mapped, &d, sizeof(d));
}

void Renderer::finishGi(FrameUBO& u) {
    std::memcpy(u.giOrigin, giGrid_[0], sizeof(u.giOrigin));
    std::memcpy(u.giDims, giGrid_[1], sizeof(u.giDims));
    std::memcpy(u.giCell, giGrid_[2], sizeof(u.giCell));
    FrameUBO* faces = reinterpret_cast<FrameUBO*>(giFaces_.data());
    for (int i = 0; i < giCount_ * 6; ++i) {
        FrameUBO& fu = faces[i];
        std::memcpy(fu.shadowViewProj, u.shadowViewProj, sizeof(u.shadowViewProj));   // (the main view's shadows)
        std::memcpy(fu.shadowRect, u.shadowRect, sizeof(u.shadowRect));
        std::memcpy(fu.cascadeSplits, u.cascadeSplits, sizeof(u.cascadeSplits));
        fu.sunDir[3] = u.sunDir[3];
        fu.ambient[3] = u.ambient[3];   // (its lights: the buffer holds the main view's)
        std::memcpy(fu.giOrigin, u.giOrigin, sizeof(u.giOrigin));   // (lit by the probes too: light bounces again)
        std::memcpy(fu.giDims, u.giDims, sizeof(u.giDims));
        std::memcpy(fu.giCell, u.giCell, sizeof(u.giCell));
        std::memcpy(static_cast<uint8_t*>(ubo_[frame_].mapped) + uboStride_ * (1 + kMaxViews + i), &fu, sizeof(fu));
    }
}

void Renderer::recordGi(VkCommandBuffer cb, const SceneRenderInfo& info) {
    if (!giOn_ || giCount_ == 0) return;
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags depthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, frag = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    imageBarrier(cb, giAtlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 compute | colorStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (giAtlasMsaa_.image)
        imageBarrier(cb, giAtlasMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, colorStage,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, giDepth_.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (giAtlasMsaa_.image) {
        color.imageView = giAtlasMsaa_.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = giAtlas_.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = giAtlas_.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = giDepth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {6u * kGiFace, (uint32_t)kGiPerFrame * kGiFace}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    vkCmdBeginRendering(cb, &ri);
    for (int n = 0; n < giCount_; ++n)
        for (int f = 0; f < 6; ++f) {
            const uint32_t slot = (uint32_t)(uboStride_ * (1 + kMaxViews + n * 6 + f));
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
            VkViewport vp{(float)(f * kGiFace), (float)(n * kGiFace), (float)kGiFace, (float)kGiFace, 0.f, 1.f};
            VkRect2D sc{{f * kGiFace, n * kGiFace}, {(uint32_t)kGiFace, (uint32_t)kGiFace}};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline_);
            VkDescriptorSet bound = VK_NULL_HANDLE;
            for (const Batch& b : giBatches_[n]) {
                if (b.texture != bound) {
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
                    bound = b.texture;
                }
                drawBatch(cb, b);
                stats_.drawCalls++;
            }
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
    vkCmdEndRendering(cb);

    // Projected into their slots (once the frames before have read the buffer), read by everything drawn after
    imageBarrier(cb, giAtlas_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, compute,
                 VK_ACCESS_SHADER_READ_BIT);
    VkBufferMemoryBarrier before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.buffer = giProbes_.buffer;
    before.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, frag | compute, compute, 0, 0, nullptr, 1, &before, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, giPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, giLayout_, 0, 1, &giSets_[frame_], 0, nullptr);
    vkCmdDispatch(cb, (uint32_t)giCount_, 1, 1);
    VkBufferMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, compute, frag, 0, 0, nullptr, 1, &after, 0, nullptr);
    const uint32_t mainSlot = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &mainSlot);
    (void)info;
}
