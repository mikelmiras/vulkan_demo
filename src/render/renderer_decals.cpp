// Decals for the shaders (Decal, render/decals.h): the nearest kMaxDecals boxes in a buffer per frame (set 0,
// binding 24) and their textures in kDecalTextures slots (binding 25), rewritten each frame in that frame's set;
// the weather's settings with them (FrameUBO::weather). scene.frag paints them before lighting.
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

#include "render/decals.h"
#include "render/renderer_internal.h"

using namespace render_detail;

void Renderer::createDecalBuffers() {
    for (int i = 0; i < MAX_FRAMES; ++i) {
        decalBuffers_[i] = createBuffer(sizeof(GpuDecal) * kMaxDecals, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkDescriptorBufferInfo bi{decalBuffers_[i].buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descSets_[i];
        w.dstBinding = 24;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
}

// This frame's decals (the main view's camera picks them; the extra views see the same) and weather
void Renderer::writeDecals(const SceneRenderInfo& info, FrameUBO& u) {
    u.weather[0] = std::clamp(info.wetness, 0.f, 1.f);
    u.weather[1] = std::clamp(info.puddles, 0.f, 1.f);
    u.weather[2] = std::clamp(info.rain, 0.f, 1.f);
    const int textureCount = (int)gpuTextures_.size();
    // (those in view: every pixel tests every decal it's given)
    std::vector<DecalInfo> inView;
    if (info.decals) {
        Frustum frustum(info.viewProj);
        for (const DecalInfo& d : *info.decals)
            if (frustum.visible(d.center, d.radius)) inView.push_back(d);
    }
    DecalPick pick = pickDecals(inView, info.camPos, kMaxDecals, kDecalTextures);
    auto* out = static_cast<GpuDecal*>(decalBuffers_[frame_].mapped);
    for (size_t k = 0; k < pick.decals.size(); ++k) {
        const DecalInfo& d = inView[(size_t)pick.decals[k]];
        GpuDecal g{};
        g.toBox = d.toBox;
        g.color[0] = d.color.x, g.color[1] = d.color.y, g.color[2] = d.color.z, g.color[3] = std::clamp(d.opacity, 0.f, 1.f);
        const int slot = pick.slots[k];
        const bool uploaded = slot >= 0 && pick.textures[(size_t)slot] < textureCount;
        g.params[0] = uploaded ? (float)slot : -1.f;
        g.params[1] = d.roughness;
        g.params[2] = std::clamp(d.wetness, 0.f, 1.f);
        g.params[3] = std::clamp(d.angleFade, 0.f, 1.f);
        g.down[0] = d.down.x, g.down[1] = d.down.y, g.down[2] = d.down.z;
        g.sphere[0] = d.center.x, g.sphere[1] = d.center.y, g.sphere[2] = d.center.z, g.sphere[3] = d.radius * d.radius;
        out[k] = g;
    }
    u.weather[3] = (float)pick.decals.size();
    // Their textures (whole: a decal's texture isn't streamed), white in the slots left
    VkDescriptorImageInfo images[kDecalTextures];
    for (uint32_t s = 0; s < kDecalTextures; ++s) {
        const GpuTexture* t = &whiteTexture_;
        if (s < pick.textures.size() && pick.textures[s] >= 0 && pick.textures[s] < textureCount) {
            t = &gpuTextures_[(size_t)pick.textures[s]];
            t->pinned = true;
        }
        images[s] = {t->nearest ? nearestSampler_ : sampler_, t->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = descSets_[frame_];
    w.dstBinding = 25;
    w.descriptorCount = kDecalTextures;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = images;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}
