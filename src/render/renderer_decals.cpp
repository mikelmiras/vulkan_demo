// Decals for the shaders (Decal, render/decals.h): the nearest kMaxDecals boxes in a buffer per frame (set 0,
// binding 24), binned into the lights' clusters (binding 27: each pixel tests its cluster's alone), their textures
// in kDecalTextures slots (binding 25) and their normal maps in kDecalNormals (binding 28), rewritten each frame in
// that frame's set; the weather's settings with them (FrameUBO::weather). scene.frag paints them before lighting.
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

#include "render/decals.h"
#include "render/renderer_internal.h"

using namespace render_detail;

void Renderer::createDecalBuffers() {
    for (int i = 0; i < MAX_FRAMES; ++i) {
        decalBuffers_[i] = createBuffer(sizeof(GpuDecal) * kMaxDecals, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const size_t clusterBytes = ((size_t)kClusterX * kClusterY * kClusterSlices * 2 + kMaxDecalIndices) * sizeof(uint32_t);
        decalClusters_[i] = createBuffer(clusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(decalClusters_[i].mapped, 0, clusterBytes);   // (no decals anywhere until the first frame)
        VkDescriptorBufferInfo bi[2] = {{decalBuffers_[i].buffer, 0, VK_WHOLE_SIZE}, {decalClusters_[i].buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2];
        for (int k = 0; k < 2; ++k) {
            w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[k].dstSet = descSets_[i];
            w[k].dstBinding = k == 0 ? 24 : 27;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
}

void Renderer::writeDecalStandIns() {
    VkDescriptorImageInfo images[kDecalTextures], normals[kDecalNormals];
    for (uint32_t s = 0; s < kDecalTextures; ++s) images[s] = {sampler_, whiteTexture_.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t s = 0; s < kDecalNormals; ++s) normals[s] = {sampler_, flatNormal_.linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = w[1].dstSet = descSets_[i];
        w[0].dstBinding = 25;
        w[0].descriptorCount = kDecalTextures;
        w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = images;
        w[1].dstBinding = 28;
        w[1].descriptorCount = kDecalNormals;
        w[1].pImageInfo = normals;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
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
    DecalPick pick = pickDecals(inView, info.camPos, kMaxDecals, kDecalTextures, kDecalNormals);
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
        const int normalSlot = pick.normalSlots[k];
        const bool normalUp = normalSlot >= 0 && pick.normalTextures[(size_t)normalSlot] < textureCount;
        g.normal[0] = normalUp ? (float)normalSlot : -1.f;
        g.normal[1] = d.normalStrength;
        out[k] = g;
    }
    u.weather[3] = (float)pick.decals.size();
    // Into the lights' clusters (the main view's, this frame): each pixel then tests the decals of its own
    if (clusterSetupValid_) {
        std::vector<ClusterLight> bounds(pick.decals.size());
        for (size_t k = 0; k < pick.decals.size(); ++k) {
            const DecalInfo& d = inView[(size_t)pick.decals[k]];
            bounds[k] = {d.center, d.radius};
        }
        std::vector<uint32_t> data;
        buildLightClusters(clusterSetup_, bounds, data, kMaxDecalIndices);
        std::memcpy(decalClusters_[frame_].mapped, data.data(), data.size() * sizeof(uint32_t));
    }
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
    VkDescriptorImageInfo normals[kDecalNormals];   // (their normal maps: plain numbers, flat in the slots left)
    for (uint32_t s = 0; s < kDecalNormals; ++s) {
        const GpuTexture* t = &flatNormal_;
        if (s < pick.normalTextures.size() && pick.normalTextures[s] >= 0 && pick.normalTextures[s] < textureCount) {
            t = &gpuTextures_[(size_t)pick.normalTextures[s]];
            t->pinned = true;
        }
        normals[s] = {t->nearest ? nearestSampler_ : sampler_, t->linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = w[1].dstSet = descSets_[frame_];
    w[0].dstBinding = 25;
    w[0].descriptorCount = kDecalTextures;
    w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = images;
    w[1].dstBinding = 28;
    w[1].descriptorCount = kDecalNormals;
    w[1].pImageInfo = normals;
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}
