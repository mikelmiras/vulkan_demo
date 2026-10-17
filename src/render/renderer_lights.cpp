// Point and spot lights for the shaders: a buffer of every light drawn (the nearest kMaxLights), clusters saying
// which of them reach each part of the view (render/light_clusters.h), and spot lights' cookies.
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/project.h"
#include "core/vfs.h"
#include "render/light_clusters.h"
#include "render/renderer_internal.h"
#include "stb_image.h"

using namespace render_detail;

namespace {
void writePlantSet(VkDevice device, VkDescriptorSet set, VkBuffer buffer);   // (binding 16)
}

void Renderer::createLightBuffers() {
    const size_t clusterBytes = ((size_t)kClusterX * kClusterY * kClusterSlices * 2 + kMaxClusterIndices) * sizeof(uint32_t);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        lights_[i] = createBuffer(sizeof(GpuLight) * kMaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        clusters_[i] = createBuffer(clusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(clusters_[i].mapped, 0, clusterBytes);   // (no lights anywhere until the first frame)
        VkDescriptorBufferInfo infos[2] = {{lights_[i].buffer, 0, VK_WHOLE_SIZE}, {clusters_[i].buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2];
        for (int k = 0; k < 2; ++k) {
            w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[k].dstSet = descSets_[i];
            w[k].dstBinding = 14 + (uint32_t)k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[k].pBufferInfo = &infos[k];
        }
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
        plantBuffers_[i] = createBuffer(1024 * 6 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        plantBuffers_[i].size = 1024 * 6 * sizeof(float);
        writePlantSet(device_, descSets_[i], plantBuffers_[i].buffer);
        plantKeys_[i] = 0;
    }
}

// ---- Foliage: the plants of every terrain, one buffer per frame (rewritten when any of them changes)
namespace {
void writePlantSet(VkDevice device, VkDescriptorSet set, VkBuffer buffer) {
    VkDescriptorBufferInfo bi{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 16;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}
}  // namespace

void Renderer::uploadPlants(const SceneRenderInfo& info) {
    plantBases_.clear();
    uint64_t key = 1469598103934665603ull;
    size_t total = 0;
    if (info.plants)
        for (const PlantSet& s : *info.plants) {
            plantBases_.push_back((uint32_t)total);
            total += s.count;
            key = (key ^ s.key) * 1099511628211ull;
            key = (key ^ (uint64_t)s.count) * 1099511628211ull;
        }
    if (key == plantKeys_[frame_]) return;   // (as this frame's buffer already holds them)
    Buffer& buf = plantBuffers_[frame_];
    size_t bytes = std::max<size_t>(total, 1) * 6 * sizeof(float);
    if (buf.size < bytes) {   // (this frame's fence has passed: its buffer and set are free)
        destroyBuffer(buf);
        size_t capacity = 1024;
        while (capacity * 6 * sizeof(float) < bytes) capacity *= 2;
        buf = createBuffer(capacity * 6 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        buf.size = capacity * 6 * sizeof(float);
        writePlantSet(device_, descSets_[frame_], buf.buffer);
    }
    if (info.plants)
        for (size_t k = 0; k < info.plants->size(); ++k) {
            const PlantSet& s = (*info.plants)[k];
            if (s.count) std::memcpy(static_cast<float*>(buf.mapped) + (size_t)plantBases_[k] * 6, s.plants, s.count * 6 * sizeof(float));
        }
    plantKeys_[frame_] = key;
}

// After the solid objects: each range of plants in view and nearer than it fades out, instanced
void Renderer::drawPlants(VkCommandBuffer cb, const SceneRenderInfo& info, VkPipeline pipeline) {
    if (!pipeline) pipeline = foliagePipeline_;
    if (!info.plants || info.plants->empty() || !pipeline) return;
    Frustum view(info.viewProj);
    struct Push {
        int32_t shadowLayer, pad[3];
        Mat4 model;
        float color[4], params[4];
    } p{};
    bool bound = false;
    for (size_t k = 0; k < info.plants->size() && k < plantBases_.size(); ++k) {
        const PlantSet& s = (*info.plants)[k];
        for (const PlantSet::Range& r : s.ranges) {
            if (r.mesh < 0 || r.mesh >= meshes_->count() || !r.count) continue;
            const Mesh& mesh = meshes_->get(r.mesh);
            // The range's box: where they stand, grown by the mesh at its biggest
            float reach = std::max({std::fabs(mesh.boundsMin.x), std::fabs(mesh.boundsMax.x), std::fabs(mesh.boundsMin.z),
                                    std::fabs(mesh.boundsMax.z), mesh.boundsMax.y, -mesh.boundsMin.y}) * r.maxScale;
            Vec3 lo = r.lo - Vec3{reach, reach, reach}, hi = r.hi + Vec3{reach, reach, reach};
            if (!view.visible(s.model, lo, hi)) continue;
            Vec3 c = transformPoint(s.model, (lo + hi) * 0.5f);
            float radius = length(transformDir(s.model, (hi - lo) * 0.5f));
            if (length(c - info.camPos) - radius > r.fade) continue;   // (all faded out)
            if (!bound) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                VkDescriptorSet plain = materialSet(DrawItem{});
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &plain, 0, nullptr);
                bound = true;
            }
            Vec3 color = srgbToLinear(r.color);
            p.model = s.model;
            p.color[0] = color.x; p.color[1] = color.y; p.color[2] = color.z;
            p.params[0] = r.wind;
            p.params[1] = std::max(r.fade, 0.1f);
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p), &p);
            vkCmdDrawIndexed(cb, mesh.indexCount, r.count, mesh.firstIndex, mesh.vertexOffset, plantBases_[k] + r.first);
            stats_.drawCalls++;
            stats_.triangles += mesh.indexCount / 3 * r.count;
        }
    }
}

// The depth range of what's in view (every object's box; all of 0.1 to 1000 m otherwise): the light clusters' slices
// and the sun's cascades fit to it
void Renderer::fitViewDepth(const SceneRenderInfo& info) {
    viewNear_ = 0.1f;
    viewFar_ = 1000.f;
    viewFloor_ = -1e9f;
    viewLo_ = {-1e9f, -1e9f, -1e9f};
    viewHi_ = {1e9f, 1e9f, 1e9f};
    if (!info.items) return;
    std::vector<std::pair<Vec3, Vec3>> boxes;
    boxes.reserve(info.items->size());
    for (const DrawItem& d : *info.items) {
        if (d.mesh < 0 || d.mesh >= meshes_->count()) continue;
        const Mesh& mesh = meshes_->get(d.mesh);
        Vec3 c = transformPoint(d.model, (mesh.boundsMin + mesh.boundsMax) * 0.5f), e = (mesh.boundsMax - mesh.boundsMin) * 0.5f;
        Vec3 r{std::fabs(d.model.m[0]) * e.x + std::fabs(d.model.m[4]) * e.y + std::fabs(d.model.m[8]) * e.z,
               std::fabs(d.model.m[1]) * e.x + std::fabs(d.model.m[5]) * e.y + std::fabs(d.model.m[9]) * e.z,
               std::fabs(d.model.m[2]) * e.x + std::fabs(d.model.m[6]) * e.y + std::fabs(d.model.m[10]) * e.z};
        boxes.push_back({c - r, c + r});
    }
    if (info.plants && !info.plants->empty()) boxes.clear();   // (plants aren't items: the whole range then)
    if (!boxes.empty()) {
        viewFloor_ = 1e9f;
        for (const auto& b : boxes) viewFloor_ = std::min(viewFloor_, b.first.y);
        // (round the visible ones: where shadows can be seen landing)
        Frustum frustum(info.viewProj);
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const auto& b : boxes)
            if (frustum.visible(Mat4{}, b.first, b.second)) {
                lo = {std::min(lo.x, b.first.x), std::min(lo.y, b.first.y), std::min(lo.z, b.first.z)};
                hi = {std::max(hi.x, b.second.x), std::max(hi.y, b.second.y), std::max(hi.z, b.second.z)};
            }
        if (lo.x <= hi.x) viewLo_ = lo, viewHi_ = hi;
    }
    fitDepthRange(info.viewProj, info.camPos, info.camForward, boxes, viewNear_, viewFar_);
}

void Renderer::writeLights(const SceneRenderInfo& info, const std::vector<const LightInfo*>& lights,
                           const std::vector<int>& shadowView, FrameUBO& u, bool clustered) {
    auto* out = static_cast<GpuLight*>(lights_[frame_].mapped);
    size_t count = std::min(lights.size(), (size_t)kMaxLights);
    for (size_t i = 0; i < count; ++i) {
        const LightInfo& l = *lights[i];
        GpuLight g{};
        Vec3 c = srgbToLinear(l.color) * l.intensity;
        Vec3 d = normalize(l.direction);
        g.posRange[0] = l.position.x; g.posRange[1] = l.position.y; g.posRange[2] = l.position.z;
        g.posRange[3] = std::max(l.range, 0.01f);
        g.color[0] = c.x; g.color[1] = c.y; g.color[2] = c.z;
        g.color[3] = l.spot ? 1.f : 0.f;
        g.dir[0] = d.x; g.dir[1] = d.y; g.dir[2] = d.z;
        g.params[1] = i < shadowView.size() ? (float)shadowView[i] : -1.f;
        g.shape[0] = std::max(l.size, 0.f);
        g.params[2] = -1.f;
        if (l.spot) {
            float outer = std::clamp(l.spotAngle, 1.f, 89.f) * DEG2RAD;
            float inner = outer * (1.f - std::clamp(l.spotSoftness, 0.f, 1.f));
            g.dir[3] = std::cos(outer);
            g.params[0] = std::cos(inner);
            g.params[3] = std::tan(outer);
            if (!l.cookie.empty())
                if (auto it = cookieLayers_.find(l.cookie); it != cookieLayers_.end()) g.params[2] = (float)it->second;
        }
        out[i] = g;
    }
    u.ambient[3] = (float)count;
    lightCount_ = (uint32_t)count;
    // The slices fitted to the depth of what's in view (fitViewDepth: the main view's, this frame)
    float near = 0.1f, far = 1000.f;
    if (clustered && info.items) {
        near = viewNear_;
        far = viewFar_;
        fogFar_ = far;   // (the volumetric fog's cells reach as far, from about half as near)
        fogNear_ = near;
    }
    u.clusterGrid[0] = (float)kClusterX;
    u.clusterGrid[1] = (float)kClusterY;
    u.clusterGrid[2] = (float)kClusterSlices;
    u.clusterGrid[3] = clustered ? 1.f : 0.f;
    u.clusterDepth[0] = near;
    u.clusterDepth[1] = (float)kClusterSlices / std::log(far / near);
    if (!clustered) return;   // (a probe view: the shader loops over every light; the main view's clusters stay)
    ClusterSetup s;
    s.viewProj = info.viewProj;
    s.camPos = info.camPos;
    s.camForward = info.camForward;
    s.near = near;
    s.far = far;
    s.tilesX = kClusterX;
    s.tilesY = kClusterY;
    s.slices = kClusterSlices;
    clusterSetup_ = s;   // (the decals go into the same clusters)
    clusterSetupValid_ = true;
    std::vector<ClusterLight> bounds(count);
    for (size_t i = 0; i < count; ++i) bounds[i] = {lights[i]->position, std::max(lights[i]->range, 0.01f)};
    std::vector<uint32_t> data;
    buildLightClusters(s, bounds, data, kMaxClusterIndices);
    std::memcpy(clusters_[frame_].mapped, data.data(), data.size() * sizeof(uint32_t));
    clusteredIndices_ = (uint32_t)(data.size() - (size_t)s.clusters() * 2);
}

// ---- Cookies: kCookieLayers images, kCookieSize square, loaded as lights name them (a layer stays with its path)
void Renderer::createCookies() {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kCookieSize, kCookieSize, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = kCookieLayers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &cookies_.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, cookies_.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &cookies_.memory));
    VK_CHECK(vkBindImageMemory(device_, cookies_.image, cookies_.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = cookies_.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kCookieLayers};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &cookies_.view));
    cookiesReady_ = false;   // (made white and readable by the first updateCookies: no command pool yet)
}

void Renderer::updateCookies(const SceneRenderInfo& info) {
    auto barrier = [&](VkCommandBuffer cb, uint32_t layer, uint32_t count, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = cookies_.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, count};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    };
    if (!cookiesReady_) {   // white: a light whose cookie is missing shines as if it had none
        VkCommandBuffer cb = beginOneTimeCommands();
        barrier(cb, 0, kCookieLayers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue white{{1.f, 1.f, 1.f, 1.f}};
        VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kCookieLayers};
        vkCmdClearColorImage(cb, cookies_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &all);
        barrier(cb, 0, kCookieLayers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endOneTimeCommands(cb);
        cookiesReady_ = true;
    }
    if (!info.lights) return;
    for (const LightInfo& l : *info.lights) {
        if (!l.spot || l.cookie.empty() || cookieLayers_.count(l.cookie)) continue;
        int layer = (int)std::count_if(cookieLayers_.begin(), cookieLayers_.end(), [](const auto& c) { return c.second >= 0; });
        if (layer >= (int)kCookieLayers) {
            logLine("cookie %s: more than %u cookies in use", l.cookie.c_str(), kCookieLayers);
            cookieLayers_[l.cookie] = -1;
            continue;
        }
        std::string bytes;
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = nullptr;
        if (vfs::read(std::filesystem::u8path(project::resolve(l.cookie)), bytes))
            pixels = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
        if (!pixels) {
            logLine("cookie %s: cannot load it", l.cookie.c_str());
            cookieLayers_[l.cookie] = -1;
            continue;
        }
        // Resampled to the layer's size (bilinear); its brightness is what the light lets through
        std::vector<uint8_t> rgba((size_t)kCookieSize * kCookieSize * 4);
        for (uint32_t y = 0; y < kCookieSize; ++y)
            for (uint32_t x = 0; x < kCookieSize; ++x) {
                float fx = ((float)x + 0.5f) * (float)w / (float)kCookieSize - 0.5f;
                float fy = ((float)y + 0.5f) * (float)h / (float)kCookieSize - 0.5f;
                int x0 = std::clamp((int)std::floor(fx), 0, w - 1), y0 = std::clamp((int)std::floor(fy), 0, h - 1);
                int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                float tx = std::clamp(fx - (float)x0, 0.f, 1.f), ty = std::clamp(fy - (float)y0, 0.f, 1.f);
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int px, int py) { return (float)pixels[((size_t)py * w + px) * 4 + c]; };
                    float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                    float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                    rgba[((size_t)y * kCookieSize + x) * 4 + c] = (uint8_t)std::lround(top + (bottom - top) * ty);
                }
            }
        stbi_image_free(pixels);
        Buffer staging = createBuffer(rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba.data());
        VkCommandBuffer cb = beginOneTimeCommands();   // (waits for the GPU: cookies arrive rarely)
        barrier(cb, (uint32_t)layer, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)layer, 1};
        copy.imageExtent = {kCookieSize, kCookieSize, 1};
        vkCmdCopyBufferToImage(cb, staging.buffer, cookies_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, (uint32_t)layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endOneTimeCommands(cb);
        destroyBuffer(staging);
        cookieLayers_[l.cookie] = layer;
    }
}
