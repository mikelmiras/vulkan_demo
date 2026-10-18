// Shadows: the sun's 4 cascades (each an orthographic camera around a slice of the view, snapped to texels so edges
// don't shimmer), in a layered image, and spot and point lights' (perspective; six for a point light) in tiles of
// one atlas: the most important lights (nearest the camera, in view) get the biggest tiles. Extra views (split
// screen) have their own cascades in the atlas.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/light_clusters.h"
#include "render/renderer_internal.h"
#include "render/shadow_cascades.h"

using namespace render_detail;

Renderer::ShadowArray Renderer::createShadowArray(uint32_t size, uint32_t count, VkFormat color) {
    ShadowArray a;
    a.size = size;
    a.count = count;
    const bool colour = color != VK_FORMAT_UNDEFINED;
    const VkFormat format = colour ? color : shadowFormat_;
    const VkImageAspectFlags aspect = colour ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {size, size, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = count;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = (colour ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT;   // (depth: the cascades' still casters copied aside and back)
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
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, count};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &a.array));
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    a.layers.resize(count);
    for (uint32_t l = 0; l < count; ++l) {
        vci.subresourceRange = {aspect, 0, 1, l, 1};
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

bool Renderer::cascadeDue(uint64_t frame, uint32_t c) {
    if (c < 2) return true;
    uint64_t period = c == 2 ? 2 : 4;
    return (frame + c) % period == 0;
}

uint64_t Renderer::computeShadows(const SceneRenderInfo& info, Vec3 sunDir, bool sunShadows,
                                  const std::vector<const LightInfo*>& lights, FrameUBO& u,
                                  std::vector<int>& shadowLayer) {
    uint64_t layers = 0;
    shadowLayer.assign(lights.size(), -1);
    shadowSunDir_ = sunDir;
    for (auto& lamp : shadowLamp_) lamp[3] = 0.f;   // (set below for the layers lamps get)
    // ---- The sun (render/shadow_cascades.h)
    float extent[kCascades][2] = {};
    auto cascades = [&](const SceneRenderInfo& v, float texels, Mat4* vps, float* splits) {
        // (from the nearest thing in view: an overhead camera's cascades all on the ground it sees; the main view's)
        const float nearest = &v == &info && fittedCascades_ ? std::max(viewNear_ * 0.9f, 0.1f) : 0.1f;
        shadows::sunCascades({v.camPos, v.camForward, v.fovY, v.aspect}, sunDir, nearest, shadowDistance(), texels,
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
    u.shadowParams[2] = quality_ == Quality::Low ? 0.f : quality_ == Quality::Medium ? 1.f : 2.f;   // (the shaders' taps)
    if (sunShadows) layers |= (1ull << kCascades) - 1;
    // The farther cascades on a schedule (the second every frame, the third every second, the fourth every fourth):
    // between, their last camera and scale are kept (what they hold was drawn with them)
    shadowDeferred_ = 0;
    if (sunShadows && shadowCacheOn_)
        for (uint32_t c = 2; c < kCascades; ++c) {
            ShadowCache& k = shadowCache_[c];
            if (!k.valid || k.empty || cascadeDue(frameCounter_, c)) continue;
            u.shadowViewProj[c] = k.viewProj;
            std::memcpy(u.cascadeScale[c], k.scale, sizeof(k.scale));
            shadowDeferred_ |= 1ull << c;
        }
    for (uint32_t c = 0; c < kCascades; ++c)   // (what a cascade drawn now is drawn with)
        if (!(shadowDeferred_ & (1ull << c))) std::memcpy(shadowCache_[c].scale, u.cascadeScale[c], sizeof(u.cascadeScale[c]));
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
            shadowLamp_[next][0] = l->position.x, shadowLamp_[next][1] = l->position.y, shadowLamp_[next][2] = l->position.z;
            shadowLamp_[next][3] = std::max(l->range, 0.2f);
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
    shadowsDrawn_ = 0;
    if (!shadowLayers_) return;   // (both stay readable, as last frame left them)
    // Which layers to draw: those whose camera, place or casters changed since they were drawn (all, without the
    // cache); kept ones aren't touched
    auto changed = [&](uint32_t layer) {
        if (shadowDeferred_ & (1ull << layer)) return false;
        const ShadowCache& k = shadowCache_[layer];
        if (!shadowCacheOn_ || !k.valid || k.empty || k.signature != shadowSignature_[layer]) return true;
        if (std::memcmp(k.viewProj.m, shadowViewProj_[layer].m, sizeof(k.viewProj.m)) != 0) return true;
        const ShadowTile& t = shadowTiles_[layer];
        return layer >= kCascades && (k.tile.x != t.x || k.tile.y != t.y || k.tile.size != t.size);
    };
    uint64_t draw = 0, clearOnly = 0;
    for (uint32_t layer = 0; layer < kShadowLayers; ++layer) {
        bool active = (shadowLayers_ >> layer) & 1;
        if (active && changed(layer)) draw |= 1ull << layer;
        // (an unused cascade reads as lit: cleared once, then left)
        if (!active && layer < kCascades && !(shadowCacheOn_ && shadowCache_[layer].valid && shadowCache_[layer].empty))
            clearOnly |= 1ull << layer;
    }
    // (cascades whose still casters are kept but which have moving ones, now or last time: those drawn again over a
    // copy of the still ones)
    uint64_t refresh = 0;
    for (uint32_t layer = 0; layer < kCascades && staticCascades_.image; ++layer) {
        const bool kept = !(((draw | clearOnly) >> layer) & 1) && !((shadowDeferred_ >> layer) & 1) && ((shadowLayers_ >> layer) & 1);
        if (kept && (!shadowMovingBatches_[layer].empty() || hadMoving_[layer])) refresh |= 1ull << layer;
    }
    const uint64_t cascadeBits = (1ull << kCascades) - 1;
    bool cascadesTouched = ((draw | clearOnly | refresh) & cascadeBits) != 0, atlasTouched = (draw >> kCascades) != 0;
    if (!cascadesTouched && !atlasTouched) return;   // (all as they were)
    std::vector<VkImage> images;
    if (cascadesTouched) images.push_back(cascades_.image);
    if (atlasTouched) images.push_back(atlas_.image);
    bool atlasUsed = atlasTouched;
    // The previous frame's sampling must finish first; what's kept stays (the layout from readable, not undefined)
    for (VkImage image : images)
        barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, depthStages,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    auto remember = [&](uint32_t layer, bool empty) {
        ShadowCache& k = shadowCache_[layer];
        k.viewProj = shadowViewProj_[layer];
        k.signature = shadowSignature_[layer];
        k.tile = shadowTiles_[layer];
        k.valid = true;
        k.empty = empty;
        ++shadowsDrawn_;
    };
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
    // The sun's cascades: a layer each (cleared and drawn; unused ones cleared, reading as "lit"). With moving casters
    // the still ones are drawn, copied aside, and the moving ones drawn over them; while the still ones are kept,
    // each frame starts from that copy
    auto layerBarrier = [&](VkImage image, uint32_t layer, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
                            VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
        vkCmdPipelineBarrier(cb, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags depthAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkImageLayout attachment = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    auto copyLayer = [&](VkImage from, VkImage to, uint32_t layer, const std::vector<std::array<int, 4>>& rects) {
        std::vector<VkImageCopy> regions;
        for (const auto& r : rects) {
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1};
            region.srcOffset = region.dstOffset = {r[0], r[1], 0};
            region.extent = {(uint32_t)(r[2] - r[0]), (uint32_t)(r[3] - r[1]), 1};
            regions.push_back(region);
        }
        if (!regions.empty())
            vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());
    };
    auto saveStill = [&](uint32_t layer) {   // (the layer as drawn, still casters alone, into the copy)
        layerBarrier(cascades_.image, layer, attachment, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthStages,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        layerBarrier(staticCascades_.image, layer, staticSaved_[layer] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, transfer, VK_ACCESS_TRANSFER_READ_BIT, transfer,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
        copyLayer(cascades_.image, staticCascades_.image, layer, {{0, 0, (int)cascades_.size, (int)cascades_.size}});
        layerBarrier(staticCascades_.image, layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     transfer, VK_ACCESS_TRANSFER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        layerBarrier(cascades_.image, layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, attachment, transfer,
                     VK_ACCESS_TRANSFER_READ_BIT, depthStages, depthAccess);
        staticSaved_[layer] = true;
    };
    auto restoreStill = [&](uint32_t layer, const std::vector<std::array<int, 4>>& rects) {   // (the copy back where the
                                                                                          //  moving casters were or are)
        layerBarrier(cascades_.image, layer, attachment, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, depthStages, depthAccess,
                     transfer, VK_ACCESS_TRANSFER_WRITE_BIT);
        copyLayer(staticCascades_.image, cascades_.image, layer, rects);
        layerBarrier(cascades_.image, layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, attachment, transfer,
                     VK_ACCESS_TRANSFER_WRITE_BIT, depthStages, depthAccess);
    };
    auto cascadePass = [&](uint32_t layer, bool clear, const std::vector<Batch>* only) {
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = cascades_.layers[layer];
        sd.imageLayout = attachment;
        sd.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {1.f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {cascades_.size, cascades_.size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cb, &ri);
        if (only) {   // (the moving casters alone)
            VkViewport vp{0.f, 0.f, (float)cascades_.size, (float)cascades_.size, 0.f, 1.f};
            VkRect2D sc{{0, 0}, {cascades_.size, cascades_.size}};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            int32_t index = (int32_t)layer;
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(index), &index);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
            for (const Batch& b : *only) drawBatch(cb, b);
        } else if (((draw | refresh) >> layer) & 1) {
            drawView(layer, 0, 0, cascades_.size);
        }
        vkCmdEndRendering(cb);
    };
    for (uint32_t layer = 0; layer < kCascades; ++layer) {
        const bool full = ((draw | clearOnly) >> layer) & 1, again = (refresh >> layer) & 1;
        if (!full && !again) continue;
        const bool moving = (((draw | refresh) >> layer) & 1) != 0 && !shadowMovingBatches_[layer].empty();
        // (its camera as last drawn: a camera on the move redraws it every frame anyway, so no copy is worth making)
        const ShadowCache& k = shadowCache_[layer];
        const bool sameCamera = k.valid && std::memcmp(k.viewProj.m, shadowViewProj_[layer].m, sizeof(k.viewProj.m)) == 0;
        if (full || !staticSaved_[layer]) {   // the still casters (or nothing: an unused cascade) drawn afresh
            cascadePass(layer, true, nullptr);
            if (full) remember(layer, !((draw >> layer) & 1));
            staticSaved_[layer] = false;
            if ((moving && sameCamera) || again) saveStill(layer);
        } else {
            // (where the moving casters are, and were last time: their old shadows go; overlapping patches merged)
            std::vector<std::array<int, 4>> rects = movingRects_[layer];
            if (hadMoving_[layer]) rects.insert(rects.end(), lastMovingRects_[layer].begin(), lastMovingRects_[layer].end());
            restoreStill(layer, shadows::mergeRects(rects));
        }
        if (moving) cascadePass(layer, false, &shadowMovingBatches_[layer]);
        hadMoving_[layer] = moving;
        lastMovingRects_[layer] = moving ? movingRects_[layer] : std::vector<std::array<int, 4>>{};
        if (again) ++shadowsDrawn_;
    }
    // ... and their tint layers (white, then what lets light through multiplied in): with them, as cached as they are
    if (cascadesTouched && shadowTintPipeline_) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = shadowTint_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        for (uint32_t layer = 0; layer < kCascades; ++layer) {
            if (!(((draw | clearOnly) >> layer) & 1)) continue;
            const bool drawn = (draw >> layer) & 1;
            VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ta.imageView = shadowTint_.layers[layer];
            ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ta.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ta.clearValue.color = {{1.f, 1.f, 1.f, 1.f}};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {shadowTint_.size, shadowTint_.size}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ta;
            vkCmdBeginRendering(cb, &ri);
            const std::vector<Batch>& casters = shadowTintBatches_[layer];
            if (drawn && !casters.empty()) {
                VkViewport vp{0.f, 0.f, (float)shadowTint_.size, (float)shadowTint_.size, 0.f, 1.f};
                VkRect2D sc{{0, 0}, {shadowTint_.size, shadowTint_.size}};
                vkCmdSetViewport(cb, 0, 1, &vp);
                vkCmdSetScissor(cb, 0, 1, &sc);
                int32_t index = (int32_t)layer;
                vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(index), &index);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTintPipeline_);
                VkDescriptorSet bound = VK_NULL_HANDLE;
                for (const Batch& bt : casters) {
                    if (bt.texture != bound) {
                        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &bt.texture, 0, nullptr);
                        bound = bt.texture;
                    }
                    drawBatch(cb, bt);
                }
            }
            vkCmdEndRendering(cb);
            tinted_[layer] = drawn && !casters.empty();
        }
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,   // (the fog reads it too)
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // The atlas: one pass, each view cleared and drawn in its tile (the rest is never read)
    if (atlasUsed) {
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = atlas_.layers[0];
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // (the tiles not drawn again keep theirs)
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {atlas_.size, atlas_.size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cb, &ri);
        for (uint32_t layer = kCascades; layer < kShadowLayers; ++layer) {
            if (!((draw >> layer) & 1)) continue;
            const ShadowTile& t = shadowTiles_[layer];
            VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
            clear.clearValue.depthStencil = {1.f, 0};
            VkClearRect rect{{{(int32_t)t.x, (int32_t)t.y}, {t.size, t.size}}, 0, 1};
            vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
            drawView(layer, t.x, t.y, t.size);
            remember(layer, false);
        }
        vkCmdEndRendering(cb);
        // ... and the lamps' tint, in the same tiles at a quarter of the size: each drawn lamp's cleared white, then
        // what lets light through multiplied in (the split-screen views' cascades have none)
        if (shadowTintPipeline_ && lampTint_.image) {
            const float k = (float)lampTint_.size / (float)atlas_.size;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = lampTint_.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ta.imageView = lampTint_.layers[0];
            ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ta.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // (the tiles not drawn again keep theirs)
            ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            tri.renderArea = {{0, 0}, {lampTint_.size, lampTint_.size}};
            tri.layerCount = 1;
            tri.colorAttachmentCount = 1;
            tri.pColorAttachments = &ta;
            vkCmdBeginRendering(cb, &tri);
            for (uint32_t layer = kCascades; layer < kShadowLayers; ++layer) {
                if (!((draw >> layer) & 1) || shadowLamp_[layer][3] <= 0.f) continue;
                const ShadowTile& t = shadowTiles_[layer];
                const uint32_t x = (uint32_t)(t.x * k), y = (uint32_t)(t.y * k), size = std::max((uint32_t)(t.size * k), 1u);
                VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
                clear.clearValue.color = {{1.f, 1.f, 1.f, 1.f}};
                VkClearRect rect{{{(int32_t)x, (int32_t)y}, {size, size}}, 0, 1};
                vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
                const std::vector<Batch>& casters = shadowTintBatches_[layer];
                tinted_[layer] = !casters.empty();
                if (casters.empty()) continue;
                VkViewport vp{(float)x, (float)y, (float)size, (float)size, 0.f, 1.f};
                VkRect2D sc{{(int32_t)x, (int32_t)y}, {size, size}};
                vkCmdSetViewport(cb, 0, 1, &vp);
                vkCmdSetScissor(cb, 0, 1, &sc);
                int32_t index = (int32_t)layer;
                vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(index), &index);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTintPipeline_);
                VkDescriptorSet bound = VK_NULL_HANDLE;
                for (const Batch& bt : casters) {
                    if (bt.texture != bound) {
                        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &bt.texture, 0, nullptr);
                        bound = bt.texture;
                    }
                    drawBatch(cb, bt);
                }
            }
            vkCmdEndRendering(cb);
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
        }
    }
    for (VkImage image : images)
        barrier(image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,   // (the fog reads them too)
                VK_ACCESS_SHADER_READ_BIT);
    stats_.shadowViewsDrawn = shadowsDrawn_;
}
