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
    }
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
    const float near = 0.1f, far = 1000.f;
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
