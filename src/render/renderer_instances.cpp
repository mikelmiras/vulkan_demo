// Per-frame instance data: frustum culling, batching by mesh and material, and skinning matrices.
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

#include "render/renderer_internal.h"

using namespace render_detail;

void Renderer::ensureInstanceCapacity(size_t count) {
    Buffer& buf = instances_[frame_];
    if (buf.buffer && buf.size >= count * sizeof(InstanceData)) return;
    // This frame's fence has been waited on: its old buffer is no longer in use, nor is its descriptor set
    if (buf.buffer) destroyBuffer(buf);
    size_t capacity = 1024;
    while (capacity < count) capacity *= 2;
    buf = createBuffer(capacity * sizeof(InstanceData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    buf.size = capacity * sizeof(InstanceData);
    VkDescriptorBufferInfo bi{buf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = descSets_[frame_];
    w.dstBinding = 2;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void Renderer::uploadJoints(const SceneRenderInfo& info) {
    size_t count = info.joints ? info.joints->size() : 0;
    Buffer& buf = joints_[frame_];
    if (!buf.buffer || buf.size < std::max<size_t>(count, 1) * sizeof(Mat4)) {
        // This frame's fence has been waited on: its old buffer and descriptor set are free
        if (buf.buffer) destroyBuffer(buf);
        size_t capacity = 64;
        while (capacity < count) capacity *= 2;
        buf = createBuffer(capacity * sizeof(Mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        buf.size = capacity * sizeof(Mat4);
        VkDescriptorBufferInfo bi{buf.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descSets_[frame_];
        w.dstBinding = 3;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    if (count) std::memcpy(buf.mapped, info.joints->data(), count * sizeof(Mat4));
}

void Renderer::prepareInstances(const SceneRenderInfo& info) {
    sceneBatches_.clear();
    for (auto& b : shadowBatches_) b.clear();
    outlineBatches_.clear();
    stats_ = {};
    if (!info.items) return;
    const std::vector<DrawItem>& items = *info.items;
    Frustum view(info.viewProj);
    std::vector<Frustum> shadowViews;
    std::vector<uint32_t> shadowLayers;
    for (uint32_t l = 0; l < 8; ++l)
        if (shadowLayers_ & (1u << l)) {
            shadowViews.emplace_back(shadowViewProj_[l]);
            shadowLayers.push_back(l);
        }
    std::vector<std::vector<uint32_t>> casters(shadowLayers.size());

    auto textureSet = [&](const DrawItem& o) { return materialSet(o); };
    // Visible items, sorted so that equal (texture, mesh) pairs sit together and can be drawn as one batch
    std::vector<uint32_t> visible;
    visible.reserve(items.size());
    for (uint32_t i = 0; i < (uint32_t)items.size(); ++i) {
        const DrawItem& o = items[i];
        const Mesh& mesh = meshes_->get(o.mesh);
        // Skinned meshes can move anywhere their skeleton takes them: never culled
        bool skinned = o.jointOffset >= 0;
        if (skinned || view.visible(o.model, mesh.boundsMin, mesh.boundsMax)) visible.push_back(i);
        else stats_.culled++;
        for (size_t k = 0; k < shadowViews.size(); ++k)   // each shadow camera: what it sees
            if (skinned || shadowViews[k].visible(o.model, mesh.boundsMin, mesh.boundsMax)) casters[k].push_back(i);
    }
    std::stable_sort(visible.begin(), visible.end(), [&](uint32_t a, uint32_t b) {
        VkDescriptorSet ta = textureSet(items[a]), tb = textureSet(items[b]);
        if (ta != tb) return ta < tb;
        return items[a].mesh < items[b].mesh;
    });
    size_t casterCount = 0;
    for (auto& list : casters) {
        std::stable_sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) { return items[a].mesh < items[b].mesh; });
        casterCount += list.size();
    }
    size_t outlined = 0;
    for (const DrawItem& o : items) outlined += o.outlined;

    ensureInstanceCapacity(visible.size() + casterCount + outlined);
    InstanceData* out = static_cast<InstanceData*>(instances_[frame_].mapped);
    uint32_t n = 0;
    auto append = [&](std::vector<Batch>& batches, const DrawItem& o, Vec3 rgb, VkDescriptorSet set) {
        bool textured = o.texture >= 0 && o.texture < (int)gpuTextures_.size();
        out[n] = {o.model, {rgb.x, rgb.y, rgb.z, 1.f},
                  {o.checker ? 1.f : 0.f, o.tiling, textured ? 1.f : 0.f, (float)o.jointOffset},
                  {o.metallic, o.roughness, o.normalTexture >= 0 ? o.normalScale : 0.f, 0.f},
                  {o.emissive.x, o.emissive.y, o.emissive.z, 0.f}};
        if (!batches.empty() && batches.back().mesh == o.mesh && batches.back().texture == set &&
            batches.back().first + batches.back().count == n)
            batches.back().count++;
        else
            batches.push_back({o.mesh, set, n, 1});
        ++n;
    };
    for (uint32_t i : visible) append(sceneBatches_, items[i], srgbToLinear(items[i].color), textureSet(items[i]));
    VkDescriptorSet plain = materialSet(DrawItem{});
    for (size_t k = 0; k < casters.size(); ++k)
        for (uint32_t i : casters[k]) append(shadowBatches_[shadowLayers[k]], items[i], {}, plain);
    for (const DrawItem& o : items)   // each outline is drawn on its own (stencil, then hull): no merging
        if (o.outlined) {
            std::vector<Batch> single;
            append(single, o, kOutlineSrgb, textureSet(o));
            outlineBatches_.push_back(single[0]);
        }
    stats_.objects = (uint32_t)visible.size();
    stats_.shadowCasters = (uint32_t)casterCount;
}

void Renderer::uploadParticles(const SceneRenderInfo& info) {
    struct Gpu {
        float posSize[4], color[4];
    };
    std::vector<const ParticleDraw*> alpha, additive;
    if (info.particles)
        for (const ParticleDraw& p : *info.particles) (p.additive ? additive : alpha).push_back(&p);
    // See-through ones blend correctly only back to front
    std::sort(alpha.begin(), alpha.end(), [&](const ParticleDraw* a, const ParticleDraw* b) {
        Vec3 da = a->position - info.camPos, db = b->position - info.camPos;
        return dot(da, da) > dot(db, db);
    });
    size_t count = alpha.size() + additive.size();
    Buffer& buf = particles_[frame_];
    if (!buf.buffer || buf.size < std::max<size_t>(count, 1) * sizeof(Gpu)) {
        // This frame's fence has been waited on: its old buffer and descriptor set are free
        if (buf.buffer) destroyBuffer(buf);
        size_t capacity = 256;
        while (capacity < count) capacity *= 2;
        buf = createBuffer(capacity * sizeof(Gpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        buf.size = capacity * sizeof(Gpu);
        VkDescriptorBufferInfo bi{buf.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descSets_[frame_];
        w.dstBinding = 4;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    Gpu* out = static_cast<Gpu*>(buf.mapped);
    size_t n = 0;
    for (const auto* list : {&alpha, &additive})
        for (const ParticleDraw* p : *list) {
            out[n++] = {{p->position.x, p->position.y, p->position.z, p->size}, {p->color[0], p->color[1], p->color[2], p->color[3]}};
        }
    particleAlpha_ = (uint32_t)alpha.size();
    particleAdditive_ = (uint32_t)additive.size();
}
