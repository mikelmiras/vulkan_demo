// Textures (an sRGB and a UNORM view each), material descriptor sets, and textures for Dear ImGui.
#include "render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "imgui_impl_vulkan.h"
#include "render/renderer_internal.h"

using namespace render_detail;

namespace {

void imageBarrier(VkCommandBuffer cb, VkImage image, uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

// Uploads RGBA8 sRGB pixels to a device-local image with a full mip chain (when the format can be blitted). `data`
// (a normal or metal / rough map): the image is UNORM, so the blits average its bytes as numbers, not as colours
Renderer::GpuTexture Renderer::createTexture(const uint8_t* rgba, uint32_t width, uint32_t height, VkCommandBuffer frame,
                                             bool mipmaps, uint32_t first, bool data) {
    const VkFormat format = data ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu_, format, &fp);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    bool chain = mipmaps && (fp.optimalTilingFeatures & blit) == blit;
    const uint32_t fullMips = chain ? streaming::mipCount(width, height) : 1;
    first = std::min(first, fullMips - 1);
    std::vector<uint8_t> smaller;   // (streaming: the finest level held, box-filtered down from the whole image)
    if (first > 0) {
        smaller = streaming::downsample(rgba, width, height, first, width, height, data);
        rgba = smaller.data();
    }
    uint32_t mips = fullMips - first;

    GpuTexture t;
    t.first = first;
    t.mips = fullMips;
    t.data = data;
    // Mutable format: an sRGB view for colour and a UNORM view of the same bytes for data (normal maps)
    t.image = createImage(format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, width, height, VK_SAMPLE_COUNT_1_BIT, mips,
                          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    t.linear = createView(t.image.image, data ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_ASPECT_COLOR_BIT, mips);
    if (data) std::swap(t.image.view, t.linear);   // (image.view stays the sRGB one, linear the UNORM one)
    Buffer staging = createBuffer((VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba);

    auto barrier = [&](VkCommandBuffer cb, uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                       VkAccessFlags dstAccess) {
        imageBarrier(cb, t.image.image, mip, count, from, to, srcStage, srcAccess, dstStage, dstAccess);
    };
    VkCommandBuffer cb = frame ? frame : beginOneTimeCommands();
    barrier(cb, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int32_t w = (int32_t)width, h = (int32_t)height;
    for (uint32_t m = 1; m < mips; ++m) {   // each level is a filtered half-size copy of the previous one
        barrier(cb, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        region.srcOffsets[1] = {w, h, 1};
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        region.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cb, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        barrier(cb, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    barrier(cb, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    if (frame) {
        retired_[frame_].push_back(staging);
    } else {
        endOneTimeCommands(cb);
        destroyBuffer(staging);
    }
    return t;
}

Renderer::GpuTexture Renderer::createTexture(const Texture& tex, VkCommandBuffer frame, uint32_t first, bool data) {
    if (tex.cooked()) return createCookedTexture(tex, frame, first);   // (its levels made as cooking chose)
    return createTexture(tex.rgba.data(), tex.width, tex.height, frame, tex.mipmaps, first, data);
}

// Cooked levels (assets/cooking.h) copied in as they are: from `first` down, no blits
Renderer::GpuTexture Renderer::createCookedTexture(const Texture& tex, VkCommandBuffer frame, uint32_t first) {
    const uint32_t fullMips = (uint32_t)tex.levels.size();
    first = std::min(first, fullMips - 1);
    const uint32_t mips = fullMips - first;
    bc::Format format = tex.format;
    const bool decode = format != bc::Format::RGBA8 && !bcSupported_;   // (no BC on this GPU: plain pixels)
    if (decode) format = bc::Format::RGBA8;
    VkFormat srgb = VK_FORMAT_R8G8B8A8_SRGB, unorm = VK_FORMAT_R8G8B8A8_UNORM;
    if (format == bc::Format::BC1) { srgb = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; unorm = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; }
    if (format == bc::Format::BC3) { srgb = VK_FORMAT_BC3_SRGB_BLOCK; unorm = VK_FORMAT_BC3_UNORM_BLOCK; }
    if (format == bc::Format::BC7) { srgb = VK_FORMAT_BC7_SRGB_BLOCK; unorm = VK_FORMAT_BC7_UNORM_BLOCK; }

    // The levels one after the other in the staging buffer
    std::vector<uint8_t> bytes;
    std::vector<VkBufferImageCopy> copies;
    for (uint32_t m = first; m < fullMips; ++m) {
        uint32_t w = std::max(tex.width >> m, 1u), h = std::max(tex.height >> m, 1u);
        VkBufferImageCopy copy{};
        copy.bufferOffset = bytes.size();
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - first, 0, 1};
        copy.imageExtent = {w, h, 1};
        copies.push_back(copy);
        if (decode) {
            std::vector<uint8_t> px = bc::decode(tex.format, tex.levels[m].data(), w, h);
            bytes.insert(bytes.end(), px.begin(), px.end());
        } else {
            bytes.insert(bytes.end(), tex.levels[m].begin(), tex.levels[m].end());
        }
    }
    GpuTexture t;
    t.first = first;
    t.mips = fullMips;
    t.blockBytes = bc::blockBytes(format);
    t.image = createImage(srgb, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                          std::max(tex.width >> first, 1u), std::max(tex.height >> first, 1u), VK_SAMPLE_COUNT_1_BIT, mips,
                          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    t.linear = createView(t.image.image, unorm, VK_IMAGE_ASPECT_COLOR_BIT, mips);
    Buffer staging = createBuffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bytes.data());
    VkCommandBuffer cb = frame ? frame : beginOneTimeCommands();
    imageBarrier(cb, t.image.image, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cb, staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(),
                           copies.data());
    imageBarrier(cb, t.image.image, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT);
    if (frame) {
        retired_[frame_].push_back(staging);
    } else {
        endOneTimeCommands(cb);
        destroyBuffer(staging);
    }
    return t;
}

void Renderer::destroyTexture(GpuTexture& t) {
    if (t.linear) vkDestroyImageView(device_, t.linear, nullptr);
    destroyImage(t.image);   // the descriptor set goes with its pool
    t = {};
}

void Renderer::reuploadChangedTextures() {
    std::vector<size_t> changed;
    for (size_t i = 0; i < gpuTextures_.size(); ++i)
        if (gpuTextures_[i].revision != textures_->get((int)i).revision) changed.push_back(i);
    if (changed.empty()) return;
    // Rare (an import setting changed in the editor): wait for every frame using them, then swap them in place
    vkDeviceWaitIdle(device_);
    for (size_t i : changed) {
        const Texture& tex = textures_->get((int)i);
        GpuTexture old = gpuTextures_[i];
        destroyTexture(gpuTextures_[i]);
        gpuTextures_[i] = createTexture(tex, VK_NULL_HANDLE, old.first, old.data);
        gpuTextures_[i].revision = tex.revision;
        gpuTextures_[i].nearest = tex.nearest;
        gpuTextures_[i].pinned = old.pinned || !tex.mipmaps;
        gpuTextures_[i].wanted = old.wanted;
        gpuTextures_[i].lastWanted = old.lastWanted;
        // The UI's set for it keeps its handle (this frame's UI already uses it) and now shows the new image
        if (auto it = uiSets_.find((int)i); it != uiSets_.end()) {
            VkDescriptorImageInfo info{VK_NULL_HANDLE, gpuTextures_[i].linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = it->second;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   // (the backend's layout: see ImGui_ImplVulkan_AddTexture)
            w.pImageInfo = &info;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }
    vkResetDescriptorPool(device_, materialPool_, 0);   // material sets pointed at the old images: made again
    materialSets_.clear();
    terrainSets_.clear();
}

void Renderer::uploadTextures(VkCommandBuffer frame) {
    const size_t kFrameBudget = 24u << 20;   // bytes of pixels per frame (at least one texture)
    size_t sent = 0;
    for (GpuTexture& t : retiredTextures_[frame_]) destroyTexture(t);   // (replaced while this frame index was last
    retiredTextures_[frame_].clear();                                   //  in flight: no frame uses them now)
    while ((int)gpuTextures_.size() < textures_->count() && (!frame || sent < kFrameBudget)) {
        const Texture& tex = textures_->get((int)gpuTextures_.size());
        // Streamed: starting with the levels up to startSize across, the finer ones once something needs them
        uint32_t first = streaming_ && tex.mipmaps ? streaming::levelForSize(tex.width, tex.height, streamSettings_.startSize) : 0;
        gpuTextures_.push_back(createTexture(tex, frame, first));
        gpuTextures_.back().revision = tex.revision;
        gpuTextures_.back().nearest = tex.nearest;
        gpuTextures_.back().pinned = !tex.mipmaps;
        const GpuTexture& g = gpuTextures_.back();
        sent += streaming::chainBytes(tex.width, tex.height, g.first, g.mips, g.blockBytes);
    }
    if (frame) {
        remakeDataTextures(frame);
        if (streaming_) streamTextures(frame);   // (reads this frame's reports, then starts them again)
        ensureFeedback(gpuTextures_.size());
    }
    // The shadow map starts out readable, so frames without shadows can still bind it
    if (!shadowMapReady_) {
        VkCommandBuffer cb = beginOneTimeCommands();
        for (VkImage image : {cascades_.image, atlas_.image}) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
        }
        endOneTimeCommands(cb);
        shadowMapReady_ = true;
    }
}

// ---------------------------------------------------------------------------------------------
// Texture streaming
// ---------------------------------------------------------------------------------------------
void Renderer::ensureFeedback(size_t textures) {
    Buffer& b = feedback_[frame_];
    size_t need = std::max<size_t>(textures, 1) * sizeof(uint32_t);
    if (b.buffer && b.size >= need) return;
    if (b.buffer) destroyBuffer(b);   // (this frame index's: its last use is done)
    size_t capacity = 1024;
    while (capacity * sizeof(uint32_t) < need) capacity *= 2;
    b = createBuffer(capacity * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    b.size = capacity * sizeof(uint32_t);
    std::memset(b.mapped, 0xFF, b.size);   // (nothing reported)
    VkDescriptorBufferInfo bi{b.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = descSets_[frame_];
    w.dstBinding = 19;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
}

void Renderer::streamTextures(VkCommandBuffer cb) {
    static const auto started = std::chrono::steady_clock::now();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const size_t n = gpuTextures_.size();
    // What the shaders reported when this frame index was last drawn (its fence has passed), then a fresh start
    if (Buffer& fb = feedback_[frame_]; fb.buffer) {
        auto* reports = static_cast<uint32_t*>(fb.mapped);
        size_t entries = std::min(n, (size_t)(fb.size / sizeof(uint32_t)));
        for (size_t i = 0; i < entries; ++i)
            if (reports[i] != ~0u) {
                gpuTextures_[i].wanted = reports[i];
                gpuTextures_[i].lastWanted = now;
            }
        for (size_t i = 0; i < n && i < streamLead_.size(); ++i) {   // (a material's other maps: as its main one)
            int lead = streamLead_[i];
            if (lead >= 0 && (size_t)lead < n && lead != (int)i && gpuTextures_[(size_t)lead].lastWanted == now) {
                gpuTextures_[i].wanted = gpuTextures_[(size_t)lead].wanted;
                gpuTextures_[i].lastWanted = now;
            }
        }
        std::memset(reports, 0xFF, fb.size);
    }
    // What each should hold, and the changes: finer ones first, a few megabytes of pixels a frame
    std::vector<streaming::TextureState> states(n);
    for (size_t i = 0; i < n; ++i) {
        const GpuTexture& g = gpuTextures_[i];
        const Texture& tex = textures_->get((int)i);
        streaming::TextureState& s = states[i];
        s.width = tex.width;
        s.height = tex.height;
        s.mips = g.mips;
        s.resident = g.first;
        s.wanted = g.wanted == ~0u ? g.mips : g.wanted;
        s.lastWanted = g.lastWanted;
        s.pinned = g.pinned || g.mips <= 1;
        s.blockBytes = g.blockBytes;
    }
    std::vector<uint32_t> target = streaming::plan(states, streamSettings_, now);
    std::vector<size_t> changes;
    for (size_t i = 0; i < n; ++i)
        if (target[i] != gpuTextures_[i].first) changes.push_back(i);
    std::stable_sort(changes.begin(), changes.end(), [&](size_t a, size_t b) {
        bool finerA = target[a] < gpuTextures_[a].first, finerB = target[b] < gpuTextures_[b].first;
        return finerA != finerB ? finerA : target[a] < target[b];
    });
    const uint64_t kFrameBytes = 16u << 20;
    uint64_t sent = 0;
    for (size_t i : changes) {
        if (sent > 0 && sent >= kFrameBytes) break;
        const Texture& tex = textures_->get((int)i);
        GpuTexture old = gpuTextures_[i];
        GpuTexture fresh = createTexture(tex, cb, target[i], old.data);
        fresh.revision = old.revision;
        fresh.nearest = old.nearest;
        fresh.pinned = old.pinned;
        fresh.wanted = old.wanted;
        fresh.lastWanted = old.lastWanted;
        gpuTextures_[i] = fresh;
        retiredTextures_[frame_].push_back(old);   // (this frame's draws may still use it: gone when it comes round)
        sent += streaming::chainBytes(tex.width, tex.height, target[i], fresh.mips, fresh.blockBytes);
        // Material sets naming it are made again (pointing at the new image)
        const uint16_t id = (uint16_t)(i + 1);
        for (auto* cache : {&materialSets_, &terrainSets_})
            for (auto it = cache->begin(); it != cache->end();) it = it->first.uses(id) ? cache->erase(it) : std::next(it);
    }
    residentTextureBytes_ = 0;
    for (size_t i = 0; i < n; ++i) {
        const Texture& tex = textures_->get((int)i);
        residentTextureBytes_ += streaming::chainBytes(tex.width, tex.height, gpuTextures_[i].first, gpuTextures_[i].mips,
                                                       gpuTextures_[i].blockBytes);
    }
    stats_.textureBytes = residentTextureBytes_;
    stats_.textureBudget = streamSettings_.budget;
}

// A texture first met in a data slot (a normal, metal / rough, height or detail map) had its mips made as colours:
// made again next frame (remakeDataTextures). A texture also used as a colour keeps the data mips (rare).
void Renderer::markData(int id) {
    if (id < 0 || id >= (int)gpuTextures_.size() || gpuTextures_[(size_t)id].data) return;
    if (textures_->get(id).cooked()) return;   // (cooked: its levels are as the export made them)
    if (std::find(dataWanted_.begin(), dataWanted_.end(), id) == dataWanted_.end()) dataWanted_.push_back(id);
}

void Renderer::remakeDataTextures(VkCommandBuffer frame) {
    for (int id : dataWanted_) {
        GpuTexture& old = gpuTextures_[(size_t)id];
        if (old.data) continue;
        GpuTexture fresh = createTexture(textures_->get(id), frame, old.first, true);
        fresh.revision = old.revision;
        fresh.nearest = old.nearest;
        fresh.pinned = old.pinned;
        fresh.wanted = old.wanted;
        fresh.lastWanted = old.lastWanted;
        retiredTextures_[frame_].push_back(old);   // (draws in flight may still use it: gone when this index comes round)
        old = fresh;
        const uint16_t key = (uint16_t)(id + 1);   // (the sets naming it: made again, pointing at the new image)
        for (auto* cache : {&materialSets_, &terrainSets_})
            for (auto it = cache->begin(); it != cache->end();) it = it->first.uses(key) ? cache->erase(it) : std::next(it);
    }
    dataWanted_.clear();
}

VkDescriptorSet Renderer::materialSet(const DrawItem& o) {
    auto valid = [&](int id) { return id >= 0 && id < (int)gpuTextures_.size() ? id : -1; };
    int ids[6] = {valid(o.texture), valid(o.normalTexture), valid(o.metalRoughTexture), valid(o.emissiveTexture),
                  valid(o.heightTexture), valid(o.detailTexture)};
    int normals[4] = {-1, -1, -1, -1};
    if (o.terrain) {   // (four colour layers: sRGB views, white where there's none; their normal maps, flat)
        for (int k = 0; k < 4; ++k) {
            ids[k] = valid(o.layerTextures[k]);
            normals[k] = valid(o.layerNormals[k]);
            markData(normals[k]);
        }
        ids[4] = ids[5] = -1;
    } else {
        for (int k : {1, 2, 4, 5}) markData(ids[k]);
    }
    MaterialKey key;
    for (int k = 0; k < 6; ++k) key.ids[k] = (uint16_t)(ids[k] + 1);
    for (int k = 0; k < 4; ++k) key.ids[6 + k] = (uint16_t)(normals[k] + 1);
    auto& cache = o.terrain ? terrainSets_ : materialSets_;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = materialPool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &materialSetLayout_;
    if (vkAllocateDescriptorSets(device_, &dsai, &set) != VK_SUCCESS) {   // out of sets: reuse the plainest one
        return materialSets_.empty() ? VK_NULL_HANDLE : materialSets_.begin()->second;
    }
    ++materialSetsMade_;
    auto pick = [&](int id, bool colour, const GpuTexture& fallback) {
        const GpuTexture& t = id >= 0 ? gpuTextures_[(size_t)id] : fallback;
        return VkDescriptorImageInfo{t.nearest ? nearestSampler_ : sampler_, colour ? t.image.view : t.linear,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    VkDescriptorImageInfo infos[6] = {pick(ids[0], true, whiteTexture_), pick(ids[1], false, flatNormal_),
                                      pick(ids[2], false, whiteTexture_), pick(ids[3], true, whiteTexture_),
                                      pick(ids[4], false, whiteTexture_), pick(ids[5], false, flatNormal_)};
    if (o.terrain)
        for (int k = 0; k < 4; ++k) infos[k] = pick(ids[k], true, whiteTexture_);
    VkDescriptorImageInfo layerNormals[4];
    for (int k = 0; k < 4; ++k) layerNormals[k] = pick(normals[k], false, flatNormal_);
    VkWriteDescriptorSet w[7];
    for (uint32_t i = 0; i < 7; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = i == 6 ? 4 : 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = i == 6 ? layerNormals : &infos[i];
    }
    vkUpdateDescriptorSets(device_, 7, w, 0, nullptr);
    cache.emplace(key, set);
    return set;
}

ImTextureID Renderer::uiTexture(int id) const {
    // The ImGui backend takes its own kind of descriptor set (a sampled image; it brings the sampler): made for each
    // texture the first time the UI shows it, from the UNORM view (the UI's colours are already as they should look)
    if (id < 0 || id >= (int)gpuTextures_.size() || !imguiInitialized_) return (ImTextureID)0;
    const GpuTexture& t = gpuTextures_[(size_t)id];
    t.pinned = true;   // (whole from now on: the UI's descriptor keeps pointing at this image)
    if (t.first != 0) return (ImTextureID)0;   // (streamed in smaller: the whole one comes in a frame or two)
    auto it = uiSets_.find(id);
    if (it == uiSets_.end())
        it = uiSets_.emplace(id, ImGui_ImplVulkan_AddTexture(gpuTextures_[(size_t)id].linear,
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)).first;
    return (ImTextureID)it->second;
}
