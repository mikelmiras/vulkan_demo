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
    skyMultiple_ = createImage(VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_IMAGE_ASPECT_COLOR_BIT, kSkyMultipleSize,
                               kSkyMultipleSize, VK_SAMPLE_COUNT_1_BIT, 1);
    skyMultiplePipeline_ = createPostPipeline("sky_multiple.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false);
    skyTransmittancePipeline_ = createPostPipeline("sky_transmittance.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false);
    skyViewPipeline_ = createPostPipeline("sky_view.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false);
    // (one set for the multiple-scattering pass, which reads the transmittance, and the sky pass, which reads both)
    VkDescriptorImageInfo infos[2] = {{postSampler_, skyTransmittance_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {postSampler_, skyMultiple_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2];
    for (uint32_t i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = skyViewSet_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    skyFresh_ = true;
    skyHaze_ = -1.f;
}

void Renderer::destroySky() {
    for (VkPipeline p : {skyTransmittancePipeline_, skyViewPipeline_, skyMultiplePipeline_}) vkDestroyPipeline(device_, p, nullptr);
    destroyImage(skyMultiple_);
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
        atmosphere::skyHarmonics(toSun, haze, skyHarmonics_);
        skyLightSun_ = toSun;
        skyLightHaze_ = haze;
    }
    // The sun's light above the air (what the tables are for one of), times kSkyGain (the engine's sun of
    // intensity 1 is dim besides the sky's real brightness: about the default sky colours'); its brightness on top
    constexpr float kSkyGain = 5.f;
    const Vec3 outer = srgbToLinear(sun.color) * (sun.intensity * kSkyGain * std::max(info.skyIntensity, 0.f));
    auto times = [](Vec3 a, Vec3 b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; };
    const Vec3 night = Vec3{0.0004f, 0.0006f, 0.0012f} * std::max(info.skyIntensity, 0.f);   // (starlight, airglow)
    horizon = times(skyLight_.horizon, outer) + night;
    zenith = times(skyLight_.zenith, outer) + night;
    // Night: the moon (opposite the sun: a full one) takes over as the light once the sun's well down, dim and blue,
    // dimmed by the air it crosses too
    const Vec3 sunHere = times(srgbToLinear(sun.color) * sun.intensity, skyLight_.sun);
    const Vec3 toMoon = toSun * -1.f;
    const float dark = std::clamp((-toSun.y - 0.03f) / 0.08f, 0.f, 1.f);
    const bool moonlit = dark > 0.f && toMoon.y > 0.f;
    if (moonlit) {
        const Vec3 moon = Vec3{0.55f, 0.62f, 0.8f} * (0.02f * dark * sun.intensity) * std::max(info.skyIntensity, 0.f);
        sun.direction = toMoon * -1.f;
        skySunHere_ = times(moon, atmosphere::transmittance(atmosphere::kViewHeight, toMoon.y, haze));
    } else {
        skySunHere_ = sunHere;
    }
    // The clouds: as much of the sky as they cover (on average) is their colour (common.glsl's skyColor: lit by the
    // sun, and by the sky), and the sun reaching the ground is dimmed by their shadows (cloudShadow's) on average
    const float cover = std::clamp(info.cloudCover, 0.f, 1.f);
    if (cover != cloudCoverFor_) {
        cloudFraction_ = atmosphere::cloudFraction(cover);
        cloudCoverFor_ = cover;
    }
    const Vec3 cloud = skySunHere_ * ((moonlit ? 0.5f : 0.15f) * (1.f - 0.5f * cloudFraction_)) + (horizon + zenith) * 0.6f;
    // The ground: what it reflects (its colour) of the sun's light reaching it and the sky's
    const Vec3 lit = sunHere * (std::max(toSun.y, 0.f) / PI * (1.f - 0.75f * cloudFraction_)) + (horizon + zenith) * 0.5f;
    ground = times(srgbToLinear(info.ground), lit);
    // The sky's light on the scene as spherical harmonics (the ambient: brighter towards the sun's side, blue
    // overhead, greyer under clouds), the ground's below the horizon; as irradiance (convolved with the cosine:
    // Ramamoorthi & Hanrahan)
    static float lower[9];   // (the lower half-sphere's own harmonics, once)
    static bool lowerMade = false;
    if (!lowerMade) {
        atmosphere::hemisphereHarmonics(false, lower);
        lowerMade = true;
    }
    Vec3 skySh[9];
    for (int k = 0; k < 9; ++k) skySh[k] = times(skyHarmonics_[k], outer);
    atmosphere::cloudyHarmonics(skySh, cloudFraction_, cloud);
    const float band[9] = {PI, 2.f * PI / 3.f, 2.f * PI / 3.f, 2.f * PI / 3.f, PI / 4.f, PI / 4.f, PI / 4.f, PI / 4.f, PI / 4.f};
    for (int k = 0; k < 9; ++k) {
        Vec3 L = skySh[k] + ground * lower[k];
        if (k == 0) L += night * (2.f * std::sqrt(PI));   // (starlight all round: its constant term)
        set4(u.sh[k], L * band[k], 0.f);
    }
    sun.color = {1, 1, 1};   // (the colour, linear, is skySunHere_: set by the caller in sunColor)
    sun.intensity = std::max(skySunHere_.x, std::max(skySunHere_.y, skySunHere_.z)) > 1e-5f ? 1.f : 0.f;
    set4(u.atmosphere, {1.f, 1.f, moonlit ? 1.f : 0.f}, std::clamp(info.cloudCover, 0.f, 1.f));
    set4(u.skySun, outer, 0.f);
    set4(u.skyToSun, toSun, 0.f);
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
        float multiple[8] = {skyHazeNow_, 1.f / kSkyMultipleSize, 1.f / kSkyMultipleSize, 0.f};
        draw(skyMultiple_, {kSkyMultipleSize, kSkyMultipleSize}, skyMultiplePipeline_, multiple, true);
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
