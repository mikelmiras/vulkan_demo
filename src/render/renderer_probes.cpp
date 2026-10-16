// Reflection probes: the scene rendered from a point in six directions (no shadows, no probes, no particles), a face
// a frame in the frame's own commands, read back, turned into a panorama and prefiltered like a sky image on a worker
// thread (render/environment.h); set 0, bindings 9-12.
#include "render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#include "core/profiler.h"
#include "render/probe_schedule.h"
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

// The bake's next face, planned for this frame: the scene from the probe (no shadows, no probes: their maps follow
// the main camera), its frame data in its own slot (filled before the main view's: the lights it writes then are the
// ones kept, and the face takes them, finishProbeFace)
static void planFace(const SceneRenderInfo& info, const Vec3& pos, int face, uint32_t size, SceneRenderInfo& f) {
    f = info;
    f.views = nullptr;
    f.camPos = pos;
    f.camForward = environment::probeFaceForward(face);
    f.fovY = 90.f * DEG2RAD;
    f.aspect = 1.f;
    f.zNear = 0.05f;
    f.zFar = 1000.f;
    f.viewProj = perspective(f.fovY, 1.f, f.zNear, f.zFar, true) *
                 lookAt(pos, pos + environment::probeFaceForward(face), environment::probeFaceUp(face));
    f.viewportX = f.viewportY = 0.f;
    f.viewportW = f.viewportH = (float)size;
}

void Renderer::finishProbeFace(const FrameUBO& u) {
    if (probeFace_ < 0) return;
    probeFaceUbo_->ambient[3] = u.ambient[3];   // (its lights: the buffer holds the main view's)
    std::memcpy(static_cast<uint8_t*>(ubo_[frame_].mapped) + uboStride_ * kProbeSlot, probeFaceUbo_, sizeof(FrameUBO));
}

// The planned face, in the frame's own commands: drawn into the bake's target, copied into its slice of the read-back
// buffer (read on the host once this frame's fence is passed)
void Renderer::recordProbeFace(VkCommandBuffer cb) {
    if (probeFace_ < 0 || !rebake_.active) return;
    ProbeBake& b = rebake_;
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (b.msaa.image)
        imageBarrier(cb, b.msaa.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);   // (after the last face's)
    imageBarrier(cb, b.resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    const VkPipelineStageFlags depthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    imageBarrier(cb, b.depth.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
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
    const uint32_t slot = (uint32_t)(uboStride_ * kProbeSlot);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    auto draw = [&](const Batch& bt) {
        if (bt.texture != bound) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &bt.texture, 0, nullptr);
            bound = bt.texture;
        }
        drawBatch(cb, bt);
        stats_.drawCalls++;
    };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline_);
    for (const Batch& bt : probeBatches_) draw(bt);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
    vkCmdDraw(cb, 3, 1, 0, 0);
    if (!probeTransparent_.empty()) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline_);
        for (const Batch& bt : probeTransparent_) draw(bt);
    }
    vkCmdEndRendering(cb);
    imageBarrier(cb, b.resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = (VkDeviceSize)b.size * b.size * 8 * (VkDeviceSize)probeFace_;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {b.size, b.size, 1};
    vkCmdCopyImageToBuffer(cb, b.resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.readback.buffer, 1, &region);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};   // (the host reads it after the fence)
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = b.readback.buffer;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
    const uint32_t mainSlot = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &mainSlot);
    b.face = probeFace_ + 1;
    b.lastDrawn = frameCounter_;
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

// A bake under way (a new probe, or one whose sky changed), after the frame's fence: the next face planned for this
// frame (drawn in its commands: recordProbeFace); once the last face's frame is done (its fence passed), the panorama
// and prefiltering on a worker thread; once that's ready, the picture taken (finishBake). The old picture of a
// rebake, or the sky's for a new probe, is drawn with meanwhile; a replaced picture is destroyed once no frame in
// flight can read it.
void Renderer::advanceRebake(const SceneRenderInfo& info) {
    for (auto it = retiredProbes_.begin(); it != retiredProbes_.end();)
        if (it->second <= frameCounter_) {
            destroyImage(it->first);
            it = retiredProbes_.erase(it);
        } else {
            ++it;
        }
    probeFace_ = -1;
    if (!rebake_.active) return;
    PROFILE_SCOPE("Bake reflection probe");
    const bool drawnDone = probes::facesDone(frameCounter_, rebake_.lastDrawn, MAX_FRAMES);   // (none in flight uses its targets)
    bool wanted = false;
    if (info.probes)
        for (const ProbeInfo& p : *info.probes) wanted = wanted || p.key == rebake_.probe.key;
    if (!wanted) {   // (gone meanwhile: dropped, once nothing in flight draws into it)
        if (!drawnDone) return;
        if (rebake_.env.valid()) rebake_.env.wait();
        endBake(rebake_);
        return;
    }
    if (rebake_.face < 6) {
        SceneRenderInfo f;
        planFace(info, rebake_.probe.position, rebake_.face, rebake_.size, f);
        probeFaceData_.assign(sizeof(FrameUBO), 0);
        probeFaceUbo_ = reinterpret_cast<FrameUBO*>(probeFaceData_.data());
        fillFrameUBO(f, *probeFaceUbo_, false);   // (no shadows, no probes: their maps follow the main camera)
        probeFace_ = rebake_.face;
        probeFacePos_ = f.camPos;
        probeFaceViewProj_ = f.viewProj;
        return;
    }
    if (!rebake_.env.valid()) {
        if (drawnDone)
            rebake_.env = std::async(std::launch::async, environment::fromProbeFaces, readFaces(rebake_.readback.mapped, rebake_.size),
                                     rebake_.size);
        return;
    }
    if (rebake_.env.wait_for(std::chrono::seconds(0)) == std::future_status::ready) finishBake();
}

void Renderer::finishBake() {
    int found = -1;
    for (size_t i = 0; i < probes_.size(); ++i)
        if (probes_[i].key == rebake_.probe.key) found = (int)i;
    if (found < 0) {   // a new probe: added (the least recently used one not in use now evicted to make room)
        if (probes_.size() >= 8) {
            std::vector<uint64_t> used;
            std::vector<bool> active;
            for (const BakedProbe& p : probes_) {
                used.push_back(p.lastUsed);
                bool on = false;
                for (const ProbeInfo& q : activeProbeInfo_) on = on || q.key == p.key;
                active.push_back(on);
            }
            const int oldest = probes::evictionVictim(used, active);
            if (oldest >= 0) {
                retiredProbes_.push_back({probes_[(size_t)oldest].radiance, frameCounter_ + MAX_FRAMES + 1});
                probes_.erase(probes_.begin() + oldest);
            }
        }
        probes_.push_back(BakedProbe{});
        found = (int)probes_.size() - 1;
        probes_.back().key = rebake_.probe.key;
        probes_.back().lastUsed = frameCounter_;
    } else {
        retiredProbes_.push_back({probes_[(size_t)found].radiance, frameCounter_ + MAX_FRAMES + 1});
    }
    BakedProbe& p = probes_[(size_t)found];
    takeProbe(rebake_.env.get(), p);
    p.sky = rebake_.sky;
    endBake(rebake_);
    activeProbes_.clear();   // (indices into probes_ may have moved: the active ones looked up again)
    std::vector<ProbeInfo> still;
    for (const ProbeInfo& q : activeProbeInfo_)
        for (size_t i = 0; i < probes_.size(); ++i)
            if (probes_[i].key == q.key) {
                activeProbes_.push_back((int)i);
                still.push_back(q);
            }
    activeProbeInfo_ = still;
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
    bool started = false;
    const atmosphere::SkyState sky = skyStateOf(info);
    for (const ProbeInfo* p : nearest) {
        int found = -1;
        for (size_t i = 0; i < probes_.size(); ++i)
            if (probes_[i].key == p->key) found = (int)i;
        // New (or changed): baked over the next frames (advanceRebake: a face a frame, no wait), the sky's
        // reflection meanwhile; or baked under another sky (the time of day, the clouds): baked again the same way,
        // keeping its picture until the new one's ready. One bake at a time, the nearest probe first
        if (!started && !rebake_.active && (found < 0 || atmosphere::skyChangedEnough(probes_[(size_t)found].sky, sky))) {
            startBake(*p, rebake_);
            rebake_.sky = sky;
            rebake_.fresh = found < 0;
            started = true;
        }
        if (found < 0) continue;
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
