// The physically based sky (render/atmosphere.h, shaders/atmosphere.glsl): its two look-up tables, drawn by
// fragment passes before the frame's scene. The transmittance table (how much light gets through the air) only
// changes with the haze; the sky around the viewer (sky_view.frag) when the sun moves. The scene's shaders read the
// sky table (set 0, binding 23: skyColor in common.glsl); the frame's sky colours, sun colour and air come from the
// CPU's copy of the model (applyAtmosphere).
#include "render/renderer.h"

#include "render/atmosphere.h"
#include "render/renderer_internal.h"

#include <algorithm>
#include <cmath>

using namespace render_detail;

void Renderer::createSky() {
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    skyTransmittance_ = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, kSkyTransmittanceW,
                                    kSkyTransmittanceH, VK_SAMPLE_COUNT_1_BIT, 1);
    skyView_ = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, kSkyViewW, kSkyViewH,
                           VK_SAMPLE_COUNT_1_BIT, 1);
    skyTransmittancePipeline_ = createPostPipeline("sky_transmittance.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false);
    skyViewPipeline_ = createPostPipeline("sky_view.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false);
    VkDescriptorImageInfo info{postSampler_, skyTransmittance_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = skyViewSet_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    skyFresh_ = true;
    skyHaze_ = -1.f;
}

void Renderer::destroySky() {
    for (VkPipeline p : {skyTransmittancePipeline_, skyViewPipeline_}) vkDestroyPipeline(device_, p, nullptr);
    destroyImage(skyTransmittance_);
    destroyImage(skyView_);
}

// The frame's sky from the air: the sun turned to the time of day, its colour once through the air, the sky's colours
// (which light the scene: the ambient's hemisphere, the fog) and the air's thickness for aerial perspective
void Renderer::applyAtmosphere(const SceneRenderInfo& info, LightInfo& sun, Vec3& horizon, Vec3& zenith, Vec3& ground,
                               FrameUBO& u) {
    if (info.timeOfDay >= 0.f)
        sun.direction = atmosphere::sunDirection(info.timeOfDay, info.latitude, info.skyRotation) * -1.f;
    const Vec3 toSun = normalize(sun.direction) * -1.f;
    const float haze = std::clamp(info.haze, 0.f, 20.f);
    // (the same sun and haze as last time: the sums kept)
    if (dot(toSun, skyLightSun_) < 0.999999f || haze != skyLightHaze_) {
        skyLight_ = atmosphere::skyLight(toSun, haze);
        skyLightSun_ = toSun;
        skyLightHaze_ = haze;
    }
    // The sun's light above the air (what the tables are for one of), times kSkyGain (standing in for the light
    // scattered many times, which the tables only roughly have); the sky's brightness on top
    constexpr float kSkyGain = 5.f;
    const Vec3 outer = srgbToLinear(sun.color) * (sun.intensity * kSkyGain * std::max(info.skyIntensity, 0.f));
    auto times = [](Vec3 a, Vec3 b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; };
    const Vec3 night = Vec3{0.0004f, 0.0006f, 0.0012f} * std::max(info.skyIntensity, 0.f);   // (starlight, airglow)
    horizon = times(skyLight_.horizon, outer) + night;
    zenith = times(skyLight_.zenith, outer) + night;
    // The ground: what it reflects (its colour) of the sun's light reaching it and the sky's
    const Vec3 sunHere = times(srgbToLinear(sun.color) * sun.intensity, skyLight_.sun);
    const Vec3 lit = sunHere * (std::max(toSun.y, 0.f) / PI) + (horizon + zenith) * 0.5f;
    ground = times(srgbToLinear(info.ground), lit);
    sun.color = {1, 1, 1};   // (the colour, linear, is sunHere: set below in sunColor)
    sun.intensity = std::max(sunHere.x, std::max(sunHere.y, sunHere.z)) > 1e-4f ? 1.f : 0.f;
    skySunHere_ = sunHere;
    set4(u.atmosphere, {1.f, 0.f, 0.f}, 0.f);
    set4(u.skySun, outer, 0.f);
    // The air near the ground per metre (aerial perspective: as real at 1)
    const Vec3 air = atmosphere::seaLevelExtinction(haze) * (std::max(info.aerialPerspective, 0.f) * 0.001f);
    set4(u.air, air, 0.f);
    skyToSun_ = toSun;
    skyHazeNow_ = haze;
}

// The tables, before anything reads them this frame: the transmittance when the haze changed, the sky when the sun
// moved (each drawn whole by one pass)
void Renderer::recordSky(VkCommandBuffer cb) {
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (skyFresh_) {   // (readable from the first frame: binding 23 always holds the sky table, used or not)
        for (VkImage image : {skyTransmittance_.image, skyView_.image})
            mipBarrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, readable, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                       fragStage, VK_ACCESS_SHADER_READ_BIT);
        skyFresh_ = false;
    }
    if (!skyOn_) return;
    auto draw = [&](const Image& image, VkExtent2D size, VkPipeline pipeline, const float* push, bool withSet) {
        mipBarrier(cb, image.image, 0, 1, readable, target, fragStage, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = image.view;
        att.imageLayout = target;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, size};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cb, &ri);
        VkViewport vp{0.f, 0.f, (float)size.width, (float)size.height, 0.f, 1.f};
        VkRect2D sc{{0, 0}, size};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (withSet) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &skyViewSet_, 0, nullptr);
        vkCmdPushConstants(cb, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        mipBarrier(cb, image.image, 0, 1, target, readable, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage,
                   VK_ACCESS_SHADER_READ_BIT);
    };
    bool viewDue = dot(skyToSun_, skyViewSun_) < 0.999999f || !skyViewMade_;
    if (skyHazeNow_ != skyHaze_) {
        float push[8] = {skyHazeNow_, 1.f / kSkyTransmittanceW, 1.f / kSkyTransmittanceH, 0.f};
        draw(skyTransmittance_, {kSkyTransmittanceW, kSkyTransmittanceH}, skyTransmittancePipeline_, push, false);
        skyHaze_ = skyHazeNow_;
        viewDue = true;
    }
    if (viewDue) {
        float push[8] = {skyToSun_.x, skyToSun_.y, skyToSun_.z, skyHazeNow_, 1.f / kSkyViewW, 1.f / kSkyViewH, 0.f, 0.f};
        draw(skyView_, {kSkyViewW, kSkyViewH}, skyViewPipeline_, push, true);
        skyViewSun_ = skyToSun_;
        skyViewMade_ = true;
    }
}
