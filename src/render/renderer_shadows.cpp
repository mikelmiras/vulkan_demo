// Shadows: the sun's 4 cascades (each an orthographic camera around a slice of the view, snapped to texels so edges
// don't shimmer), in a layered image, and spot and point lights' (perspective; six for a point light) in tiles of
// one atlas: the most important lights (nearest the camera, in view) get the biggest tiles. Extra views (split
// screen) have their own cascades in the atlas.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>

#include "render/light_clusters.h"
#include "render/renderer_internal.h"
#include "render/shadow_cascades.h"

using namespace render_detail;

Renderer::ShadowArray Renderer::createShadowArray(uint32_t size, uint32_t count) {
    ShadowArray a;
    a.size = size;
    a.count = count;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = shadowFormat_;
    ci.extent = {size, size, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = count;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &a.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, a.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &a.memory));
    VK_CHECK(vkBindImageMemory(device_, a.image, a.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = a.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = shadowFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, count};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &a.array));
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    a.layers.resize(count);
    for (uint32_t l = 0; l < count; ++l) {
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, l, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &a.layers[l]));
    }
    return a;
}

void Renderer::destroyShadowArray(ShadowArray& a) {
    for (VkImageView v : a.layers) vkDestroyImageView(device_, v, nullptr);
    vkDestroyImageView(device_, a.array, nullptr);
    vkDestroyImage(device_, a.image, nullptr);
    vkFreeMemory(device_, a.memory, nullptr);
    a = {};
}

uint64_t Renderer::computeShadows(const SceneRenderInfo& info, Vec3 sunDir, bool sunShadows,
                                  const std::vector<const LightInfo*>& lights, FrameUBO& u,
                                  std::vector<int>& shadowLayer) {
    uint64_t layers = 0;
    shadowLayer.assign(lights.size(), -1);
    // ---- The sun (render/shadow_cascades.h)
    float extent[kCascades][2] = {};
    auto cascades = [&](const SceneRenderInfo& v, float texels, Mat4* vps, float* splits) {
        shadows::sunCascades({v.camPos, v.camForward, v.fovY, v.aspect}, sunDir, shadowDistance(), texels,
                             sunShadows ? vps : nullptr, splits, extent);
    };
    cascades(info, (float)kCascadeSize, u.shadowViewProj, u.cascadeSplits);
    for (uint32_t c = 0; c < kCascades; ++c) {   // (the soft shadows' scale: penumbrae are sized in metres)
        u.cascadeScale[c][0] = std::max(extent[c][0], 1e-3f);
        u.cascadeScale[c][1] = std::max(extent[c][1], 1e-3f);
        u.cascadeScale[c][2] = (float)kCascadeSize;
    }
    u.shadowParams[0] = info.shadowSoftness;
    u.shadowParams[1] = aa_ == Antialiasing::Temporal ? 1.f : 0.f;   // (the taps turn per pixel only where TAA blends them)
    if (sunShadows) layers |= (1ull << kCascades) - 1;
    // ---- Spot and point lights casting shadows, in view, most important first (the order given): a tile each
    // for spots, six for point lights (a cube around them, a little wider than 90 degrees per face, so filtering
    // near an edge stays inside). Tiles shrink down the list: 1024, then 512 from the third light, 256 from the
    // seventh (point lights' faces half that), while views and atlas room last.
    Frustum view(info.viewProj);
    struct Wanted {
        size_t light;
        uint32_t first, faces;
    };
    std::vector<Wanted> wanted;
    std::vector<int> sizes;
    // Split screen's other players (and the editor's game view) get cascades of their own, in the atlas before any
    // light: a quarter of it, in tiles of 1024 for one view, 512 for more
    size_t extra = info.views && sunShadows ? std::min(info.views->size(), (size_t)kMaxViews) : 0;
    for (size_t v = 0; v < extra; ++v)
        for (uint32_t c = 0; c < kCascades; ++c) sizes.push_back(extra == 1 ? 1024 : 512);
    uint32_t views = 0, rank = 0;
    const uint32_t maxViews = kViewCascades - kCascades;
    for (size_t i = 0; i < lights.size(); ++i) {
        const LightInfo* l = lights[i];
        if (!l->castShadows || l->intensity <= 0.f) continue;
        Vec3 r{l->range, l->range, l->range};
        if (!view.visible(Mat4{}, l->position - r, l->position + r)) continue;   // (lights nothing on screen)
        uint32_t faces = l->spot ? 1 : 6;
        if (views + faces > maxViews) continue;
        int size = rank < 2 ? 1024 : rank < 6 ? 512 : 256;
        if (!l->spot) size /= 2;
        if (quality_ != Quality::High) size /= 2;
        ++rank;
        wanted.push_back({i, views, faces});
        for (uint32_t f = 0; f < faces; ++f) sizes.push_back(size);
        views += faces;
    }
    std::vector<AtlasTile> tiles = allocateAtlas((int)kAtlasSize, sizes);
    const float inv = 1.f / (float)kAtlasSize;
    for (size_t v = 0; v < kMaxViews; ++v) {
        viewShadowed_[v] = false;
        if (v >= extra) continue;
        const AtlasTile* t = &tiles[v * kCascades];
        bool placed = true;
        for (uint32_t c = 0; c < kCascades; ++c) placed = placed && t[c].size > 0;
        Mat4 vps[kCascades];
        cascades((*info.views)[v], (float)(placed ? t[0].size : 1), vps, viewSplits_[v]);
        if (!placed) continue;   // (the atlas is full: player 1's, as before)
        viewShadowed_[v] = true;
        for (uint32_t c = 0; c < kCascades; ++c) {
            uint32_t layer = kViewCascades + (uint32_t)v * kCascades + c;
            shadowTiles_[layer] = {(uint32_t)t[c].x, (uint32_t)t[c].y, (uint32_t)t[c].size};
            u.shadowRect[layer][0] = (float)t[c].x * inv;
            u.shadowRect[layer][1] = (float)t[c].y * inv;
            u.shadowRect[layer][2] = u.shadowRect[layer][3] = (float)t[c].size * inv;
            u.shadowViewProj[layer] = vps[c];
            layers |= 1ull << layer;
        }
    }
    tiles.erase(tiles.begin(), tiles.begin() + (long)(extra * kCascades));
    uint32_t next = kCascades;
    for (const Wanted& w : wanted) {
        bool placed = true;
        for (uint32_t f = 0; f < w.faces; ++f) placed = placed && tiles[w.first + f].size > 0;
        if (!placed) continue;   // (the atlas is full: no shadow)
        const LightInfo* l = lights[w.light];
        shadowLayer[w.light] = (int)next;
        for (uint32_t f = 0; f < w.faces; ++f, ++next) {
            const AtlasTile& t = tiles[w.first + f];
            shadowTiles_[next] = {(uint32_t)t.x, (uint32_t)t.y, (uint32_t)t.size};
            u.shadowRect[next][0] = (float)t.x * inv;
            u.shadowRect[next][1] = (float)t.y * inv;
            u.shadowRect[next][2] = u.shadowRect[next][3] = (float)t.size * inv;
            if (l->spot) {
                Vec3 dir = normalize(l->direction);
                Vec3 lup = std::fabs(dir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
                float fov = std::min(2.f * l->spotAngle * DEG2RAD * 1.1f, 3.f);
                u.shadowViewProj[next] = perspective(fov, 1.f, 0.1f, std::max(l->range, 0.2f), true) *
                                         lookAt(l->position, l->position + dir, lup);
            } else {
                static const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                static const Vec3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
                u.shadowViewProj[next] = perspective(95.f * DEG2RAD, 1.f, 0.05f, std::max(l->range, 0.2f), true) *
                                         lookAt(l->position, l->position + dirs[f], ups[f]);
            }
            layers |= 1ull << next;
        }
    }
    return layers;
}

void Renderer::recordShadows(VkCommandBuffer cb) {
    const VkPipelineStageFlags depthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                       VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    if (!shadowLayers_) return;   // (both stay readable, as last frame left them)
    bool atlasUsed = (shadowLayers_ >> kCascades) != 0;
    std::vector<VkImage> images{cascades_.image};
    if (atlasUsed) images.push_back(atlas_.image);
    // The previous frame's sampling must finish first; the old contents are discarded
    for (VkImage image : images)
        barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, depthStages,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    auto drawView = [&](uint32_t layer, uint32_t x, uint32_t y, uint32_t size) {
        VkViewport vp{(float)x, (float)y, (float)size, (float)size, 0.f, 1.f};
        VkRect2D sc{{(int32_t)x, (int32_t)y}, {size, size}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        int32_t index = (int32_t)layer;
        vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(index), &index);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        for (const Batch& b : shadowBatches_[layer]) {
            drawBatch(cb, b);
        }
        if (!shadowCutoutBatches_[layer].empty()) {   // holes: their texture's alpha decides, per pixel
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowCutoutPipeline_);
            VkDescriptorSet bound = VK_NULL_HANDLE;
            for (const Batch& b : shadowCutoutBatches_[layer]) {
                if (b.texture != bound) {
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
                    bound = b.texture;
                }
                drawBatch(cb, b);
            }
        }
    };
    // The sun's cascades: a layer each (every one cleared: unused ones read as "lit")
    for (uint32_t layer = 0; layer < kCascades; ++layer) {
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = cascades_.layers[layer];
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {1.f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {cascades_.size, cascades_.size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cb, &ri);
        if (shadowLayers_ & (1ull << layer)) drawView(layer, 0, 0, cascades_.size);
        vkCmdEndRendering(cb);
    }
    // The atlas: one pass, each view cleared and drawn in its tile (the rest is never read)
    if (atlasUsed) {
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = atlas_.layers[0];
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {atlas_.size, atlas_.size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cb, &ri);
        for (uint32_t layer = kCascades; layer < kShadowLayers; ++layer) {
            if (!(shadowLayers_ & (1ull << layer))) continue;
            const ShadowTile& t = shadowTiles_[layer];
            VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
            clear.clearValue.depthStencil = {1.f, 0};
            VkClearRect rect{{{(int32_t)t.x, (int32_t)t.y}, {t.size, t.size}}, 0, 1};
            vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
            drawView(layer, t.x, t.y, t.size);
        }
        vkCmdEndRendering(cb);
    }
    for (VkImage image : images)
        barrier(image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,   // (the fog reads them too)
                VK_ACCESS_SHADER_READ_BIT);
}
