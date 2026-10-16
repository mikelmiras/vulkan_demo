// Per-frame instance data: frustum culling, batching by mesh and material, and skinning matrices.
#include "render/renderer.h"

#include <algorithm>
#include <cstring>

#include "assets/mesh_lod.h"
#include "render/renderer_internal.h"

using namespace render_detail;

// The instance store: one InstanceData per draw item, on the GPU. Growing it makes a new one (the old one is freed
// once the frames using it are done) that gets everything uploaded again.
void Renderer::ensureInstanceStore(size_t count) {
    if (instanceStore_.buffer && instanceStore_.size >= count * sizeof(InstanceData)) return;
    if (instanceStore_.buffer) retired_[frame_].push_back(instanceStore_);
    size_t capacity = 1024;
    while (capacity < count) capacity *= 2;
    instanceStore_ = createDeviceBuffer(capacity * sizeof(InstanceData),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    instanceMirror_.clear();   // (nothing in it yet: the next upload sends everything)
    pendingCopies_.clear();
    ++instanceStoreGen_;
}

// This frame's set: the store (if it's a new one since) and the index list
void Renderer::ensureIndexCapacity(size_t count) {
    Buffer& idx = instanceIndex_[frame_];
    bool newList = !idx.buffer || idx.size < count * sizeof(uint32_t);
    if (newList) {
        // This frame's fence has been waited on: its old list is no longer in use, nor is its descriptor set
        if (idx.buffer) destroyBuffer(idx);
        size_t capacity = 1024;
        while (capacity < count) capacity *= 2;
        idx = createBuffer(capacity * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        idx.size = capacity * sizeof(uint32_t);
    }
    bool newStore = storeBound_[frame_] != instanceStoreGen_;
    if (!newList && !newStore) return;
    VkDescriptorBufferInfo infos[2] = {{instanceStore_.buffer, 0, VK_WHOLE_SIZE}, {idx.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = descSets_[frame_];
        w[i].dstBinding = i == 0 ? 2 : 13;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    storeBound_[frame_] = instanceStoreGen_;
    if (newList) writeCullSet();
}

// What changed goes through this frame's staging buffer: copied at the start of the next command buffer recorded
// (after earlier frames' reads of the store, before this one's)
void Renderer::recordInstanceUpload(VkCommandBuffer cb) {
    if (pendingCopies_.empty()) return;
    const VkPipelineStageFlags readers =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // (after earlier reads, and earlier frames' copies)
    before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, readers | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0,
                         nullptr, 0, nullptr);
    vkCmdCopyBuffer(cb, instanceStaging_[frame_].buffer, instanceStore_.buffer, (uint32_t)pendingCopies_.size(),
                    pendingCopies_.data());
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, readers, 0, 1, &mb, 0, nullptr, 0, nullptr);
    pendingCopies_.clear();
}

MeshLod Renderer::rangeOf(const Batch& b) const {
    const Mesh& mesh = meshes_->get(b.mesh);
    if (b.lod > 0 && b.lod <= (int)mesh.lods.size()) return mesh.lods[(size_t)b.lod - 1];
    return {mesh.firstIndex, mesh.indexCount, mesh.vertexOffset};
}

void Renderer::drawBatch(VkCommandBuffer cb, const Batch& b) {
    MeshLod r = rangeOf(b);
    vkCmdDrawIndexed(cb, r.indexCount, b.count, r.firstIndex, r.vertexOffset, b.first);
}

// Each item's motion since last frame (by its key): its model matrix then, and its joints then (appended to the
// joints buffer after this frame's), for the motion vectors; this frame's kept for the next. Items not seen last
// frame (new, or back in view) haven't moved.
void Renderer::prepareMotion(const SceneRenderInfo& info) {
    ++motionFrame_;
    const size_t n = info.items ? info.items->size() : 0;
    const size_t jointCount = info.joints ? info.joints->size() : 0;
    prevModels_.resize(n);
    prevJointOffsets_.assign(n, -1);
    prevJoints_.clear();
    // (each skinned item's joints run from its offset to the next one's in the buffer: terrains' layers in it too)
    std::vector<int> starts;
    for (size_t i = 0; i < n; ++i) {
        const DrawItem& o = (*info.items)[i];
        if (o.jointOffset >= 0) starts.push_back(o.jointOffset);
        if (o.terrain && o.terrainLayers >= 0) starts.push_back(o.terrainLayers);
    }
    std::sort(starts.begin(), starts.end());
    for (size_t i = 0; i < n; ++i) {
        const DrawItem& o = (*info.items)[i];
        prevModels_[i] = o.model;
        if (!o.key) continue;
        auto it = motion_.find(o.key);
        const bool known = it != motion_.end() && it->second.frame + 1 == motionFrame_;
        if (known) prevModels_[i] = it->second.model;
        Motion& m = known ? it->second : motion_[o.key];
        m.model = o.model;
        m.frame = motionFrame_;
        if (o.jointOffset >= 0 && (size_t)o.jointOffset < jointCount) {
            auto next = std::upper_bound(starts.begin(), starts.end(), o.jointOffset);
            const size_t end = next == starts.end() ? jointCount : std::min((size_t)*next, jointCount);
            const Mat4* now = info.joints->data() + o.jointOffset;
            const size_t count = end - (size_t)o.jointOffset;
            prevJointOffsets_[i] = (int)(jointCount + prevJoints_.size());
            if (known && m.joints.size() == count) prevJoints_.insert(prevJoints_.end(), m.joints.begin(), m.joints.end());
            else prevJoints_.insert(prevJoints_.end(), now, now + count);
            m.joints.assign(now, now + count);
        }
    }
    for (auto it = motion_.begin(); it != motion_.end();)   // (gone: forgotten)
        it = it->second.frame + 2 < motionFrame_ ? motion_.erase(it) : std::next(it);
}

void Renderer::uploadJoints(const SceneRenderInfo& info) {
    size_t now = info.joints ? info.joints->size() : 0;
    size_t count = now + prevJoints_.size();   // (and last frame's, for the motion vectors)
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
    if (now) std::memcpy(buf.mapped, info.joints->data(), now * sizeof(Mat4));
    if (!prevJoints_.empty())
        std::memcpy(static_cast<Mat4*>(buf.mapped) + now, prevJoints_.data(), prevJoints_.size() * sizeof(Mat4));
}

void Renderer::prepareInstances(const SceneRenderInfo& info, bool gpuCulling) {
    gpuCulled_ = false;
    cullBatches_[frame_] = 0;
    bool gpu = gpuCulling && gpuCulling_ && cullPipeline_;   // solid objects: culled by cull.comp, not here
    sceneBatches_.clear();
    transparentBatches_.clear();
    for (int v = 0; v < kMaxViews; ++v) {
        viewBatches_[v].clear();
        viewTransparent_[v].clear();
    }
    for (auto& b : giBatches_) b.clear();
    for (auto& b : shadowBatches_) b.clear();
    for (auto& b : shadowCutoutBatches_) b.clear();
    outlineBatches_.clear();
    stats_ = {};
    if (!info.items) return;
    const std::vector<DrawItem>& items = *info.items;
    Frustum view(info.viewProj);
    std::vector<Frustum> shadowViews;
    std::vector<uint32_t> shadowLayers;
    for (uint32_t l = 0; l < kShadowLayers; ++l)
        if ((shadowLayers_ & (1ull << l)) && !(shadowDeferred_ & (1ull << l))) {   // (kept ones: nothing to draw)
            shadowViews.emplace_back(shadowViewProj_[l]);
            shadowLayers.push_back(l);
        }
    std::vector<std::vector<uint32_t>> casters(shadowLayers.size());

    auto textureSet = [&](const DrawItem& o) { return materialSet(o); };
    // Each item's level of detail, from how big it looks from the camera (shadows use the same: cheaper casters)
    std::vector<int> lods(items.size(), 0);
    float tanHalf = std::tan(info.fovY * 0.5f);
    for (size_t i = 0; i < items.size(); ++i) {
        const Mesh& mesh = meshes_->get(items[i].mesh);
        if (mesh.lods.empty() || items[i].jointOffset >= 0) continue;
        const Mat4& m = items[i].model;
        Vec3 c = transformPoint(m, (mesh.boundsMin + mesh.boundsMax) * 0.5f);
        float scale = std::max({length(transformDir(m, {1, 0, 0})), length(transformDir(m, {0, 1, 0})),
                                length(transformDir(m, {0, 0, 1}))});
        float radius = length(mesh.boundsMax - mesh.boundsMin) * 0.5f * scale;
        lods[i] = chooseLod(radius, length(c - info.camPos), tanHalf, (int)mesh.lods.size());
    }
    // Visible items, sorted so that equal (texture, mesh) pairs sit together and can be drawn as one batch
    std::vector<uint32_t> visible, transparent;
    visible.reserve(items.size());
    for (uint32_t i = 0; i < (uint32_t)items.size(); ++i) {
        const DrawItem& o = items[i];
        const Mesh& mesh = meshes_->get(o.mesh);
        // Skinned meshes can move anywhere their skeleton takes them: never culled
        bool skinned = o.jointOffset >= 0;
        if (gpu && o.alphaMode != 2) visible.push_back(i);
        else if (skinned || view.visible(o.model, mesh.boundsMin, mesh.boundsMax)) (o.alphaMode == 2 ? transparent : visible).push_back(i);
        else stats_.culled++;
        for (size_t k = 0; k < shadowViews.size() && !o.water; ++k)   // each shadow camera: what it sees (water casts none)
            if (skinned || shadowViews[k].visible(o.model, mesh.boundsMin, mesh.boundsMax)) casters[k].push_back(i);
    }
    std::stable_sort(visible.begin(), visible.end(), [&](uint32_t a, uint32_t b) {
        VkDescriptorSet ta = textureSet(items[a]), tb = textureSet(items[b]);
        if (ta != tb) return ta < tb;
        if (items[a].mesh != items[b].mesh) return items[a].mesh < items[b].mesh;
        return !gpu && lods[a] < lods[b];   // (the GPU picks levels itself)
    });
    // See-through ones blend correctly only back to front (by their centres)
    auto centre = [&](uint32_t i) {
        const Mesh& m = meshes_->get(items[i].mesh);
        return transformPoint(items[i].model, (m.boundsMin + m.boundsMax) * 0.5f);
    };
    std::sort(transparent.begin(), transparent.end(), [&](uint32_t a, uint32_t b) {
        Vec3 da = centre(a) - info.camPos, db = centre(b) - info.camPos;
        return dot(da, da) > dot(db, db);
    });
    // Each shadow layer's casters, hashed (FNV-1a): what's drawn into it (mesh, level, material for cut-outs, where,
    // vertices rewritten in place); skinned ones change every frame
    for (size_t k = 0; k < casters.size(); ++k) {
        uint64_t h = 1469598103934665603ull ^ meshes_->version;
        auto mix = [&](const void* data, size_t size) {
            const auto* p = static_cast<const uint8_t*>(data);
            for (size_t b = 0; b < size; ++b) h = (h ^ p[b]) * 1099511628211ull;
        };
        for (uint32_t i : casters[k]) {
            const DrawItem& o = items[i];
            mix(&o.mesh, sizeof(o.mesh));
            mix(&lods[i], sizeof(int));
            mix(o.model.m, sizeof(o.model.m));
            mix(&o.alphaMode, sizeof(o.alphaMode));
            if (o.alphaMode != 0) {
                VkDescriptorSet set = textureSet(o);
                mix(&set, sizeof(set));
            }
            if (o.jointOffset >= 0) mix(&frameCounter_, sizeof(frameCounter_));
            auto rewritten = meshes_->rewritten.find((uint32_t)meshes_->get(o.mesh).vertexOffset);
            if (rewritten != meshes_->rewritten.end()) mix(&rewritten->second.second, sizeof(uint64_t));
        }
        shadowSignature_[shadowLayers[k]] = h;
    }
    size_t casterCount = 0;
    for (auto& list : casters) {   // solid ones by mesh; ones with holes after them, by material
        std::stable_sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) {
            bool ha = items[a].alphaMode != 0, hb = items[b].alphaMode != 0;
            if (ha != hb) return hb;
            if (ha) {
                VkDescriptorSet ta = textureSet(items[a]), tb = textureSet(items[b]);
                if (ta != tb) return ta < tb;
            }
            return items[a].mesh < items[b].mesh;
        });
        casterCount += list.size();
    }
    size_t outlined = 0;
    for (const DrawItem& o : items) outlined += o.outlined;
    // The extra views (the editor's game view, split-screen players): culled here against their own cameras,
    // levels of detail from them too
    size_t viewCount = info.views ? std::min(info.views->size(), (size_t)kMaxViews) : 0;
    std::vector<std::vector<uint32_t>> viewVisible(viewCount), viewTransparent(viewCount);
    std::vector<std::vector<int>> viewLods(viewCount);
    size_t viewIndices = 0;
    for (size_t v = 0; v < viewCount; ++v) {
        const SceneRenderInfo& view = (*info.views)[v];
        if (!viewImages_[v].image.image) continue;   // (not drawn)
        std::vector<int>& lodOf = viewLods[v];
        lodOf.assign(items.size(), 0);
        Frustum f(view.viewProj);
        float tanView = std::tan(view.fovY * 0.5f);
        for (uint32_t i = 0; i < (uint32_t)items.size(); ++i) {
            const DrawItem& o = items[i];
            const Mesh& mesh = meshes_->get(o.mesh);
            if (o.jointOffset < 0 && !f.visible(o.model, mesh.boundsMin, mesh.boundsMax)) continue;
            (o.alphaMode == 2 ? viewTransparent[v] : viewVisible[v]).push_back(i);
            if (!mesh.lods.empty() && o.jointOffset < 0) {
                Vec3 c = transformPoint(o.model, (mesh.boundsMin + mesh.boundsMax) * 0.5f);
                float scale = std::max({length(transformDir(o.model, {1, 0, 0})), length(transformDir(o.model, {0, 1, 0})),
                                        length(transformDir(o.model, {0, 0, 1}))});
                lodOf[i] = chooseLod(length(mesh.boundsMax - mesh.boundsMin) * 0.5f * scale, length(c - view.camPos), tanView,
                                     (int)mesh.lods.size());
            }
        }
        std::stable_sort(viewVisible[v].begin(), viewVisible[v].end(), [&](uint32_t a, uint32_t b) {
            VkDescriptorSet ta = textureSet(items[a]), tb = textureSet(items[b]);
            if (ta != tb) return ta < tb;
            if (items[a].mesh != items[b].mesh) return items[a].mesh < items[b].mesh;
            return lodOf[a] < lodOf[b];
        });
        std::sort(viewTransparent[v].begin(), viewTransparent[v].end(), [&](uint32_t a, uint32_t b) {
            Vec3 da = centre(a) - view.camPos, db = centre(b) - view.camPos;
            return dot(da, da) > dot(db, db);
        });
        viewIndices += viewVisible[v].size() + viewTransparent[v].size();
    }

    // The GI probes drawn this frame (renderer_gi.cpp): solid things near each, at their coarsest level of detail
    std::vector<std::vector<uint32_t>> giVisible((size_t)giCount_);
    for (int n = 0; n < giCount_; ++n) {
        const float reach = std::max(info.giSpacing, 0.25f) * 8.f;
        for (uint32_t i = 0; i < (uint32_t)items.size(); ++i) {
            const DrawItem& o = items[i];
            if (o.alphaMode == 2) continue;
            const Mesh& mesh = meshes_->get(o.mesh);
            Vec3 c = transformPoint(o.model, (mesh.boundsMin + mesh.boundsMax) * 0.5f);
            float scale = std::max({length(transformDir(o.model, {1, 0, 0})), length(transformDir(o.model, {0, 1, 0})),
                                    length(transformDir(o.model, {0, 0, 1}))});
            float radius = length(mesh.boundsMax - mesh.boundsMin) * 0.5f * scale;
            if (length(c - giDrawPos_[n]) - radius < reach) giVisible[(size_t)n].push_back(i);
        }
        std::stable_sort(giVisible[(size_t)n].begin(), giVisible[(size_t)n].end(), [&](uint32_t a, uint32_t b) {
            VkDescriptorSet ta = textureSet(items[a]), tb = textureSet(items[b]);
            if (ta != tb) return ta < tb;
            return items[a].mesh < items[b].mesh;
        });
        viewIndices += giVisible[(size_t)n].size();
    }

    // ---- The instances: one per item, then the outlined ones again (in the outline colour); only the ones that
    // differ from what the store holds are uploaded
    const size_t total = items.size() + outlined;
    ensureInstanceStore(total);
    // Texture streaming: the map each item reports on (its colour map, else the first it has), the others following
    auto valid = [&](int id) { return id >= 0 && id < (int)gpuTextures_.size(); };
    streamLead_.assign(gpuTextures_.size(), -1);
    auto leadOf = [&](const DrawItem& o) {
        if (o.terrain) return -1;
        for (int id : {o.texture, o.normalTexture, o.metalRoughTexture, o.emissiveTexture})
            if (valid(id)) return id;
        return -1;
    };
    for (const DrawItem& o : items) {
        if (o.terrain) {   // (terrains' layers: whole, they're tiled across everything)
            for (int id : o.layerTextures)
                if (valid(id)) gpuTextures_[(size_t)id].pinned = true;
            continue;
        }
        int lead = leadOf(o);
        for (int id : {o.normalTexture, o.metalRoughTexture, o.emissiveTexture})
            if (valid(id) && id != lead) streamLead_[(size_t)id] = lead;
        for (int id : {o.heightTexture, o.detailTexture})   // (read at other places and scales: kept whole)
            if (valid(id)) gpuTextures_[(size_t)id].pinned = true;
    }
    auto instanceOf = [&](const DrawItem& o, Vec3 rgb) {
        bool textured = o.texture >= 0 && o.texture < (int)gpuTextures_.size();
        InstanceData d{o.model, {rgb.x, rgb.y, rgb.z, 1.f},
                       {o.checker ? 1.f : 0.f, o.tiling, textured ? 1.f : 0.f, (float)o.jointOffset},
                       {o.metallic, o.roughness, o.normalTexture >= 0 ? o.normalScale : 0.f, (float)o.alphaMode},
                       {o.emissive.x, o.emissive.y, o.emissive.z, o.alphaCutoff}};
        d.color[3] = o.alphaMode == 2 ? o.opacity : 1.f;
        d.layers[0] = o.clearcoat;
        d.layers[1] = o.clearcoatRoughness;
        d.layers[2] = o.sheen;
        d.layers[3] = o.anisotropy;
        d.surface[0] = o.parallaxDepth;
        d.surface[1] = o.detailTiling;
        d.surface[2] = o.detailStrength;
        d.surface[3] = o.sheltered ? 1.f : 0.f;
        int lead = streaming_ ? leadOf(o) : -1;
        d.stream[0] = (float)lead;
        if (lead >= 0) {
            const Texture& t = textures_->get(lead);
            d.stream[1] = std::log2((float)std::max(std::max(t.width, t.height), 1u));
        }
        if (o.water) {   // (water: params.x = 3; its waves in emissive, the clock and the current in stream.yzw)
            d.params[0] = 3.f;
            d.params[3] = -1.f;
            d.emissive[0] = o.waveHeight;
            d.emissive[1] = o.waveLength;
            d.emissive[2] = o.waveDirection;
            d.emissive[3] = o.choppiness;
            d.stream[0] = -1.f;
            d.stream[1] = info.waterTime;
            d.stream[2] = o.current.x;
            d.stream[3] = o.current.z;
        }
        if (o.terrain) {   // (scene.frag's terrain mode: params.x = 2; w = -(its layers' matrix + 2), never skinned)
            d.params[0] = 2.f;
            d.params[1] = 1.f;
            d.params[3] = -(float)(o.terrainLayers + 2);
            d.material[2] = 0.f;
            d.material[3] = 0.f;
        }
        return d;
    };
    {
        const size_t stride = sizeof(InstanceData);
        if (instanceMirror_.size() < total * stride) instanceMirror_.resize(total * stride, 0xFF);   // (0xFF: "unknown")
        Buffer& staging = instanceStaging_[frame_];
        if (!staging.buffer || staging.size < total * stride) {
            if (staging.buffer) destroyBuffer(staging);   // (its frame's fence has passed)
            size_t capacity = 1024;
            while (capacity < total) capacity *= 2;
            staging = createBuffer(capacity * stride, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            staging.size = capacity * stride;
        }
        pendingCopies_.clear();
        uploadedInstances_ = 0;
        auto* stage = static_cast<uint8_t*>(staging.mapped);
        size_t outlineSlot = items.size();
        for (size_t i = 0; i < total; ++i) {
            InstanceData d;
            size_t item = i;
            if (i < items.size()) {
                d = instanceOf(items[i], srgbToLinear(items[i].color));
            } else {
                while (!items[outlineSlot - items.size()].outlined) ++outlineSlot;   // (the next outlined item)
                item = outlineSlot - items.size();
                d = instanceOf(items[item], kOutlineSrgb);
                ++outlineSlot;
            }
            // (where it was last frame: the motion vectors)
            d.prevModel = item < prevModels_.size() ? prevModels_[item] : items[item].model;
            d.motion[0] = item < prevJointOffsets_.size() ? (float)prevJointOffsets_[item] : -1.f;
            d.motion[1] = d.motion[2] = d.motion[3] = 0.f;
            uint8_t* mirror = instanceMirror_.data() + i * stride;
            if (std::memcmp(mirror, &d, stride) == 0) continue;
            std::memcpy(mirror, &d, stride);
            std::memcpy(stage + i * stride, &d, stride);
            ++uploadedInstances_;
            if (!pendingCopies_.empty() && pendingCopies_.back().dstOffset + pendingCopies_.back().size == i * stride)
                pendingCopies_.back().size += stride;   // (runs of changed ones: one copy)
            else
                pendingCopies_.push_back({i * stride, i * stride, stride});
        }
    }

    // ---- The index list: each batch's instances, by item
    ensureIndexCapacity(visible.size() * (1 + kMaxLods) + transparent.size() + casterCount + outlined + viewIndices);
    auto* index = static_cast<uint32_t*>(instanceIndex_[frame_].mapped);
    uint32_t n = 0;
    auto append = [&](std::vector<Batch>& batches, uint32_t instance, int mesh, int lod, VkDescriptorSet set) {
        index[n] = instance;
        const bool water = items[instance].water;
        if (!batches.empty() && batches.back().mesh == mesh && batches.back().texture == set && batches.back().lod == lod &&
            batches.back().water == water && batches.back().first + batches.back().count == n)
            batches.back().count++;
        else
            batches.push_back({mesh, set, n, 1, lod, water});
        ++n;
    };
    for (uint32_t i : visible) append(sceneBatches_, i, items[i].mesh, gpu ? 0 : lods[i], textureSet(items[i]));
    for (uint32_t i : transparent) append(transparentBatches_, i, items[i].mesh, lods[i], textureSet(items[i]));
    VkDescriptorSet plain = materialSet(DrawItem{});
    for (size_t k = 0; k < casters.size(); ++k)
        for (uint32_t i : casters[k]) {
            if (items[i].alphaMode == 0) append(shadowBatches_[shadowLayers[k]], i, items[i].mesh, lods[i], plain);
            else append(shadowCutoutBatches_[shadowLayers[k]], i, items[i].mesh, lods[i], textureSet(items[i]));
        }
    for (int g = 0; g < giCount_; ++g)
        for (uint32_t i : giVisible[(size_t)g]) {
            int coarsest = items[i].jointOffset < 0 ? (int)meshes_->get(items[i].mesh).lods.size() : 0;
            append(giBatches_[g], i, items[i].mesh, coarsest, textureSet(items[i]));
        }
    for (size_t v = 0; v < viewCount; ++v) {
        for (uint32_t i : viewVisible[v]) append(viewBatches_[v], i, items[i].mesh, viewLods[v][i], textureSet(items[i]));
        for (uint32_t i : viewTransparent[v]) append(viewTransparent_[v], i, items[i].mesh, viewLods[v][i], textureSet(items[i]));
    }
    uint32_t outlineInstance = (uint32_t)items.size();
    for (const DrawItem& o : items)   // each outline is drawn on its own (stencil, then hull): no merging
        if (o.outlined) {
            std::vector<Batch> single;
            append(single, outlineInstance++, o.mesh, 0, textureSet(o));
            outlineBatches_.push_back(single[0]);
        }
    listedInstances_ = n;
    stats_.objects = (uint32_t)(visible.size() + transparent.size());
    stats_.shadowCasters = (uint32_t)casterCount;
    if (gpu && !visible.empty()) {
        prepareGpuCulling(items, visible, info);
        stats_.objects = gpuVisible_ + (uint32_t)transparent.size();   // (as a frame or two ago)
        stats_.culled += (uint32_t)visible.size() > gpuVisible_ ? (uint32_t)visible.size() - gpuVisible_ : 0;
    }
}

void Renderer::uploadParticles(const SceneRenderInfo& info) {
    struct Gpu {
        float posSize[4], color[4];
        float streak[4];   // xyz: back to its tail (0: a dot), w = 1: lit
    };
    std::vector<const ParticleDraw*> alpha, additive;
    if (info.particles)
        for (const ParticleDraw& p : *info.particles) (p.additive ? additive : alpha).push_back(&p);
    // See-through ones blend correctly only back to front
    std::sort(alpha.begin(), alpha.end(), [&](const ParticleDraw* a, const ParticleDraw* b) {
        Vec3 da = a->position - info.camPos, db = b->position - info.camPos;
        return dot(da, da) > dot(db, db);
    });
    size_t count = alpha.size() + additive.size() + gpuParticleSlots_;   // (the GPU's after them)
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
            out[n++] = {{p->position.x, p->position.y, p->position.z, p->size},
                        {p->color[0], p->color[1], p->color[2], p->color[3]},
                        {p->streak.x, p->streak.y, p->streak.z, p->lit ? 1.f : 0.f}};
        }
    particleAlpha_ = (uint32_t)alpha.size();
    particleAdditive_ = (uint32_t)additive.size();
}
