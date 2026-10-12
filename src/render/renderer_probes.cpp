// Reflection probes: the scene rendered from a point in six directions (no shadows, no probes, no particles), read
// back, turned into a panorama and prefiltered like a sky image (render/environment.h); set 0, bindings 9-12.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/profiler.h"
#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

float fromHalf(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) bits = sign;                                     // (zero; denormals too: tiny)
    else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// The six views: forward and up
const Vec3 kFaceForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const Vec3 kFaceUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};

}  // namespace

bool Renderer::bakeProbe(const SceneRenderInfo& info, const ProbeInfo& probe, BakedProbe& out) {
    PROFILE_SCOPE("Bake reflection probe");
    const uint32_t size = (uint32_t)std::clamp(probe.resolution, 16, 1024);
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Image resolved = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, size, size, VK_SAMPLE_COUNT_1_BIT, 1);
    Image msaa;
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        msaa = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, size, size, samples_, 1);
    Image depth = createImage(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, dsAspect, size, size, samples_, 1);
    const VkDeviceSize faceBytes = (VkDeviceSize)size * size * 8;   // RGBA16F
    Buffer readback = createBuffer(faceBytes * 6, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    for (int face = 0; face < 6; ++face) {
        SceneRenderInfo f = info;
        f.camPos = probe.position;
        f.camForward = kFaceForward[face];
        f.fovY = 90.f * DEG2RAD;
        f.aspect = 1.f;
        f.zNear = 0.05f;
        f.zFar = 1000.f;
        f.viewProj = perspective(f.fovY, 1.f, f.zNear, f.zFar, true) *
                     lookAt(probe.position, probe.position + kFaceForward[face], kFaceUp[face]);
        f.viewportX = f.viewportY = 0.f;
        f.viewportW = f.viewportH = (float)size;
        FrameUBO u{};
        fillFrameUBO(f, u, false);   // (no shadows, no probes: their maps follow the main camera)
        std::memcpy(ubo_[frame_].mapped, &u, sizeof(u));
        prepareInstances(f);
        uploadJoints(f);

        VkCommandBuffer cb = beginOneTimeCommands();
        if (msaa.image)
            imageBarrier(cb, msaa.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(cb, resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(cb, depth.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        if (msaa.image) {
            color.imageView = msaa.view;
            color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = resolved.view;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            color.imageView = resolved.view;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAtt.imageView = depth.view;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.clearValue.depthStencil = {1.f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {size, size}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depthAtt;
        ri.pStencilAttachment = &depthAtt;
        vkCmdBeginRendering(cb, &ri);
        VkViewport viewport{0.f, 0.f, (float)size, (float)size, 0.f, 1.f};
        VkRect2D scissor{{0, 0}, {size, size}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        VkBuffer vbs[2] = {vertexBuffer_.buffer, skinBuffer_.buffer};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cb, 0, 2, vbs, offsets);
        vkCmdBindIndexBuffer(cb, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 0, nullptr);
        VkDescriptorSet bound = VK_NULL_HANDLE;
        auto draw = [&](const Batch& b) {
            if (b.texture != bound) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
                bound = b.texture;
            }
            const Mesh& mesh = meshes_->get(b.mesh);
            vkCmdDrawIndexed(cb, mesh.indexCount, b.count, mesh.firstIndex, mesh.vertexOffset, b.first);
        };
        if (info.items) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline_);
            for (const Batch& b : sceneBatches_) draw(b);
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdDraw(cb, 3, 1, 0, 0);
        if (info.items && !transparentBatches_.empty()) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline_);
            for (const Batch& b : transparentBatches_) draw(b);
        }
        vkCmdEndRendering(cb);
        imageBarrier(cb, resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = faceBytes * (VkDeviceSize)face;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {size, size, 1};
        vkCmdCopyImageToBuffer(cb, resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        endOneTimeCommands(cb);   // (waits)
    }

    // The six views -> a 512 x 256 panorama -> prefiltered
    const uint16_t* px = static_cast<const uint16_t*>(readback.mapped);
    const uint32_t pw = 512, ph = 256;
    std::vector<float> rgb((size_t)pw * ph * 3);
    for (uint32_t y = 0; y < ph; ++y)
        for (uint32_t x = 0; x < pw; ++x) {
            Vec3 d = environment::directionAt(((float)x + 0.5f) / (float)pw, ((float)y + 0.5f) / (float)ph);
            int best = 0;
            float bestDot = -2.f;
            for (int f = 0; f < 6; ++f)
                if (float k = dot(d, kFaceForward[f]); k > bestDot) bestDot = k, best = f;
            Vec3 fw = kFaceForward[best], up = kFaceUp[best], right = normalize(cross(fw, up));
            up = cross(right, fw);
            float sx = dot(d, right) / bestDot, sy = -dot(d, up) / bestDot;   // -1..1 across the view (y down)
            uint32_t ix = std::min(size - 1, (uint32_t)((sx * 0.5f + 0.5f) * (float)size));
            uint32_t iy = std::min(size - 1, (uint32_t)((sy * 0.5f + 0.5f) * (float)size));
            const uint16_t* p = px + ((size_t)best * size * size + (size_t)iy * size + ix) * 4;
            for (int c = 0; c < 3; ++c) rgb[((size_t)y * pw + x) * 3 + (size_t)c] = fromHalf(p[c]);
        }
    Environment env = environment::build("probe", pw, ph, std::move(rgb));
    std::vector<const Environment::Level*> levels;
    for (const Environment::Level& l : env.radiance) levels.push_back(&l);
    out.radiance = createFloatImage(levels);
    for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) out.sh[k][c] = env.sh[k][c];
    out.key = probe.key;

    destroyBuffer(readback);
    destroyImage(depth);
    if (msaa.image) destroyImage(msaa);
    destroyImage(resolved);
    return true;
}

void Renderer::updateProbes(const SceneRenderInfo& info) {
    ++frameCounter_;
    activeProbes_.clear();
    activeProbeInfo_.clear();
    if (!info.probes || info.probes->empty()) return;
    std::vector<const ProbeInfo*> nearest;
    for (const ProbeInfo& p : *info.probes) nearest.push_back(&p);
    std::sort(nearest.begin(), nearest.end(), [&](const ProbeInfo* a, const ProbeInfo* b) {
        return length(a->position - info.camPos) < length(b->position - info.camPos);
    });
    if (nearest.size() > 4) nearest.resize(4);
    bool waited = false;
    for (const ProbeInfo* p : nearest) {
        int found = -1;
        for (size_t i = 0; i < probes_.size(); ++i)
            if (probes_[i].key == p->key) found = (int)i;
        if (found < 0) {   // new (or changed): baked now, between frames
            if (!waited) {
                vkDeviceWaitIdle(device_);
                waited = true;
                writeEnvironmentDescriptors();   // (stand-ins while baking: an evicted probe may still be bound)
            }
            if (probes_.size() >= 8) {   // the least recently used goes
                auto oldest = std::min_element(probes_.begin(), probes_.end(), [](const BakedProbe& a, const BakedProbe& b) {
                    return a.lastUsed < b.lastUsed;
                });
                destroyImage(oldest->radiance);
                probes_.erase(oldest);
            }
            BakedProbe baked;
            if (!bakeProbe(info, *p, baked)) continue;
            probes_.push_back(std::move(baked));
            found = (int)probes_.size() - 1;
            // (indices into probes_ may have moved with an eviction: look the earlier ones up again)
            activeProbes_.clear();
            for (const ProbeInfo& q : activeProbeInfo_)
                for (size_t i = 0; i < probes_.size(); ++i)
                    if (probes_[i].key == q.key) activeProbes_.push_back((int)i);
        }
        probes_[(size_t)found].lastUsed = frameCounter_;
        activeProbes_.push_back(found);
        activeProbeInfo_.push_back(*p);
    }
}

void Renderer::writeProbeDescriptors() {
    VkDescriptorImageInfo infos[4];
    VkWriteDescriptorSet w[4];
    for (uint32_t k = 0; k < 4; ++k) {
        VkImageView view = k < activeProbes_.size() ? probes_[(size_t)activeProbes_[k]].radiance.view : envRadiance_.view;
        infos[k] = {envSampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[k].dstSet = descSets_[frame_];
        w[k].dstBinding = 9 + k;
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[k].pImageInfo = &infos[k];
    }
    vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
}

void Renderer::setProbeData(FrameUBO& u) {
    for (size_t k = 0; k < activeProbes_.size() && k < 4; ++k) {
        const ProbeInfo& p = activeProbeInfo_[k];
        const BakedProbe& b = probes_[(size_t)activeProbes_[k]];
        set4(u.probePos[k], p.position, 1.f);
        set4(u.probeMin[k], p.boxMin, 0.f);
        set4(u.probeMax[k], p.boxMax, p.boxProjection ? 1.f : 0.f);
        for (int i = 0; i < 9; ++i) set4(u.probeSh[k][i], {b.sh[i][0], b.sh[i][1], b.sh[i][2]}, 0.f);
    }
}

void Renderer::destroyProbes() {
    for (BakedProbe& p : probes_) destroyImage(p.radiance);
    probes_.clear();
}
