// Reflection probes: the scene rendered from a point in six directions (no shadows, no probes, no particles), read
// back, turned into a panorama and prefiltered like a sky image (render/environment.h); set 0, bindings 9-12.
#include "render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#include "core/profiler.h"
#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

// The sky a probe baked now is lit by: the scene's sun (turned to the time of day for the physical sky), its clouds
// and haze (the clouds and haze matter to the physical sky alone)
atmosphere::SkyState skyStateOf(const SceneRenderInfo& info) {
    atmosphere::SkyState state;
    Vec3 dir = normalize({-0.45f, -1.f, -0.3f});   // (fillFrameUBO's default sun)
    if (info.lights)
        for (const LightInfo& l : *info.lights)
            if (l.directional) {
                dir = l.direction;
                break;
            }
    state.toSun = normalize(dir) * -1.f;
    if (info.physicalSky) {
        if (info.timeOfDay >= 0.f) state.toSun = atmosphere::sunDirection(info.timeOfDay, info.latitude, info.skyRotation);
        state.cloudCover = std::clamp(info.cloudCover, 0.f, 1.f);
        state.haze = std::clamp(info.haze, 0.f, 20.f);
    }
    return state;
}

}  // namespace

// A probe's bake: its targets and the read-back buffer (six faces, RGBA16F)
void Renderer::startBake(const ProbeInfo& probe, ProbeBake& b) {
    b.probe = probe;
    b.size = (uint32_t)std::clamp(probe.resolution, 16, 1024);
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    b.resolved = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, b.size, b.size, VK_SAMPLE_COUNT_1_BIT, 1);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        b.msaa = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, b.size, b.size, samples_, 1);
    b.depth = createImage(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, dsAspect, b.size, b.size, samples_, 1);
    b.readback = createBuffer((VkDeviceSize)b.size * b.size * 8 * 6, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    b.face = 0;
    b.active = true;
}

void Renderer::endBake(ProbeBake& b) {
    destroyBuffer(b.readback);
    destroyImage(b.depth);
    if (b.msaa.image) destroyImage(b.msaa);
    destroyImage(b.resolved);
    b = ProbeBake{};
}

// One face: the scene from the probe (no shadows, no probes: their maps follow the main camera), read back (waits)
void Renderer::bakeFace(const SceneRenderInfo& info, ProbeBake& b, int face) {
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    SceneRenderInfo f = info;
    f.views = nullptr;
    f.camPos = b.probe.position;
    f.camForward = environment::probeFaceForward(face);
    f.fovY = 90.f * DEG2RAD;
    f.aspect = 1.f;
    f.zNear = 0.05f;
    f.zFar = 1000.f;
    f.viewProj = perspective(f.fovY, 1.f, f.zNear, f.zFar, true) *
                 lookAt(b.probe.position, b.probe.position + environment::probeFaceForward(face), environment::probeFaceUp(face));
    f.viewportX = f.viewportY = 0.f;
    f.viewportW = f.viewportH = (float)b.size;
    FrameUBO u{};
    fillFrameUBO(f, u, false);   // (no shadows, no probes: their maps follow the main camera)
    std::memcpy(ubo_[frame_].mapped, &u, sizeof(u));
    prepareInstances(f, false);   // (culled on the CPU: these draws aren't indirect)
    uploadJoints(f);

    VkCommandBuffer cb = beginOneTimeCommands();
    readyFreshImages(cb);   // (before the first frame: what set 0 holds isn't readable yet)
    recordInstanceUpload(cb);
    if (b.msaa.image)
        imageBarrier(cb, b.msaa.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, b.resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, b.depth.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (b.msaa.image) {
        color.imageView = b.msaa.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = b.resolved.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = b.resolved.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = b.depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {b.size, b.size}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    vkCmdBeginRendering(cb, &ri);
    VkViewport viewport{0.f, 0.f, (float)b.size, (float)b.size, 0.f, 1.f};
    VkRect2D scissor{{0, 0}, {b.size, b.size}};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    VkBuffer vbs[2] = {vertexBuffer_.buffer, skinBuffer_.buffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cb, 0, 2, vbs, offsets);
    vkCmdBindIndexBuffer(cb, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    const uint32_t slot = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    auto draw = [&](const Batch& b) {
        if (b.texture != bound) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
            bound = b.texture;
        }
        drawBatch(cb, b);
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
    imageBarrier(cb, b.resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = (VkDeviceSize)b.size * b.size * 8 * (VkDeviceSize)face;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {b.size, b.size, 1};
    vkCmdCopyImageToBuffer(cb, b.resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.readback.buffer, 1, &region);
    endOneTimeCommands(cb);   // (waits)
}

namespace {
std::vector<uint16_t> readFaces(const void* mapped, uint32_t size) {
    const uint16_t* p = static_cast<const uint16_t*>(mapped);
    return std::vector<uint16_t>(p, p + (size_t)size * size * 4 * 6);
}
}  // namespace

// The probe's picture and harmonics from a built environment
void Renderer::takeProbe(const Environment& env, BakedProbe& out) {
    std::vector<const Environment::Level*> levels;
    for (const Environment::Level& l : env.radiance) levels.push_back(&l);
    out.radiance = createFloatImage(levels);
    for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) out.sh[k][c] = env.sh[k][c];
}

bool Renderer::bakeProbe(const SceneRenderInfo& info, const ProbeInfo& probe, BakedProbe& out) {
    PROFILE_SCOPE("Bake reflection probe");
    ProbeBake b;
    startBake(probe, b);
    for (int face = 0; face < 6; ++face) bakeFace(info, b, face);
    takeProbe(environment::fromProbeFaces(readFaces(b.readback.mapped, b.size), b.size), out);
    out.key = probe.key;
    out.sky = skyStateOf(info);
    endBake(b);
    return true;
}

// A rebake under way (a probe whose sky changed): a face a frame (after the frame's fence: its buffers are free),
// then the panorama and prefiltering on a worker thread, then the new picture swapped in; the old one destroyed once
// no frame in flight can read it. Frames go on drawing with the old picture meanwhile.
void Renderer::advanceRebake(const SceneRenderInfo& info) {
    for (auto it = retiredProbes_.begin(); it != retiredProbes_.end();)
        if (it->second <= frameCounter_) {
            destroyImage(it->first);
            it = retiredProbes_.erase(it);
        } else {
            ++it;
        }
    if (!rebake_.active) return;
    PROFILE_SCOPE("Rebake reflection probe");
    int found = -1;
    for (size_t i = 0; i < probes_.size(); ++i)
        if (probes_[i].key == rebake_.probe.key) found = (int)i;
    if (found < 0) {   // (gone meanwhile: dropped)
        if (rebake_.env.valid()) rebake_.env.wait();
        endBake(rebake_);
        return;
    }
    if (rebake_.face < 6) {
        bakeFace(info, rebake_, rebake_.face++);
        if (rebake_.face == 6)
            rebake_.env = std::async(std::launch::async, environment::fromProbeFaces, readFaces(rebake_.readback.mapped, rebake_.size),
                                     rebake_.size);
        return;
    }
    if (rebake_.env.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    BakedProbe& p = probes_[(size_t)found];
    retiredProbes_.push_back({p.radiance, frameCounter_ + MAX_FRAMES + 1});
    takeProbe(rebake_.env.get(), p);
    p.sky = rebake_.sky;
    endBake(rebake_);
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
    bool waited = false, rebaked = false;
    const atmosphere::SkyState sky = skyStateOf(info);
    for (const ProbeInfo* p : nearest) {
        int found = -1;
        for (size_t i = 0; i < probes_.size(); ++i)
            if (probes_[i].key == p->key) found = (int)i;
        // Baked under another sky (the time of day, the clouds): baked again over the next frames (advanceRebake: a
        // face a frame, no stall), one at a time; it keeps its picture until the new one's ready
        if (found >= 0 && !rebaked && !rebake_.active && atmosphere::skyChangedEnough(probes_[(size_t)found].sky, sky)) {
            startBake(*p, rebake_);
            rebake_.sky = sky;
            rebaked = true;
        }
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
    if (rebake_.active) {
        if (rebake_.env.valid()) rebake_.env.wait();
        endBake(rebake_);
    }
    for (auto& r : retiredProbes_) destroyImage(r.first);
    retiredProbes_.clear();
    for (BakedProbe& p : probes_) destroyImage(p.radiance);
    probes_.clear();
}
