// Vulkan 1.3 renderer: scene (MSAA + depth/stencil), selection outline, and the ImGui overlay.
#pragma once

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/meshes.h"
#include "render/atmosphere.h"
#include "render/environment.h"
#include "render/light_clusters.h"
#include "render/display.h"
#include "render/texture_streaming.h"
#include "assets/textures.h"
#include "core/vecmath.h"

struct ImDrawData;
namespace render_detail { struct FrameUBO; }   // render/renderer_internal.h

// One mesh to draw. The renderer knows nothing about the scene: callers flatten it into these.
struct DrawItem {
    Mat4 model;
    uint32_t key = 0;           // which object it is, frame to frame (its entity's; 0: none): its motion is kept by it
    int mesh = 0;               // id in the MeshLibrary
    Vec3 color{1, 1, 1};        // sRGB; multiplies the texture
    int texture = -1;           // id in the TextureLibrary, or -1
    float tiling = 1.f;         // texture repeats
    bool checker = false;       // procedural checkerboard + grid
    bool outlined = false;      // draw the selection outline around it
    int jointOffset = -1;       // skinned: its joint matrices start here in SceneRenderInfo::joints
    // Material (see MeshRenderer)
    float metallic = 0.f, roughness = 0.5f;
    int metalRoughTexture = -1, normalTexture = -1, emissiveTexture = -1;
    float normalScale = 1.f;
    Vec3 emissive;              // linear, times its strength
    int alphaMode = 0;          // 0 opaque, 1 mask (cut out below alphaCutoff), 2 blend (see-through)
    float opacity = 1.f, alphaCutoff = 0.5f;
    float clearcoat = 0.f, clearcoatRoughness = 0.05f, sheen = 0.f, anisotropy = 0.f;
    float translucency = 0.f, subsurface = 0.f;
    Vec3 subsurfaceColor{1.f, 0.35f, 0.2f};   // (sRGB)
    int heightTexture = -1, detailTexture = -1;
    float parallaxDepth = 0.f, detailTiling = 8.f, detailStrength = 1.f;
    bool sheltered = false;
    // A terrain (scene/terrain.h): its mesh's skin weights mix four layers, each a texture (sRGB) times a tint
    bool terrain = false;
    int layerTextures[4] = {-1, -1, -1, -1};
    int terrainLayers = -1;     // its layers' tints and tilings (a matrix, column per layer) in SceneRenderInfo::joints
    // A water surface (scene/water.h): its grid moved by the waves in the vertex shader (shaders/water.glsl)
    bool water = false;
    float waveHeight = 0.f, waveLength = 8.f, waveDirection = 0.f, choppiness = 0.f;   // (direction: world, radians)
    float waterDepth = 4.f;     // to its bottom, m (the caustics' reach)
    Vec3 current;
};

// One particle to draw: a camera-facing dot
struct ParticleDraw {
    Vec3 position;
    float size = 0.1f;      // metres across
    float color[4];         // rgb linear (times its brightness), alpha
    bool additive = true;   // light adds up; else blended over what's behind
    Vec3 streak;            // a streak back from position to position - streak (0: a round dot)
    bool lit = false;       // its colour times the light reaching it (else its own glow)
};

struct LightInfo {
    bool directional = false;
    bool spot = false;
    float spotAngle = 30.f, spotSoftness = 0.2f;   // degrees, 0..1
    Vec3 position;              // point lights
    Vec3 direction{0, -1, 0};   // directional: where the light travels
    Vec3 color{1, 1, 1};        // sRGB
    float intensity = 1.f;
    float range = 15.f;
    bool castShadows = true;
    std::string cookie;         // spot lights: an image projected through the cone (a path; "" none)
};

// A reflection probe to draw with (ReflectionProbe, scene/scene.h): baked when its key is new
// A particle emitter simulated on the GPU (ParticleEmitter::gpu, collectGpuEmitters): where it is and how it looks
struct GpuEmitterInfo {
    uint32_t key = 0;   // (its entity: its particles are kept by it)
    Vec3 position, up{0, 1, 0};
    float rate = 20.f, lifetime = 1.5f, lifetimeRandom = 0.3f, speed = 2.f, speedRandom = 0.5f, spread = 25.f;
    float radius = 0.f, gravity = 0.f, drag = 0.f, startSize = 0.2f, endSize = 0.05f, stretch = 0.f;
    Vec3 startColor{1, 1, 1}, endColor{1, 1, 1};   // linear, times the brightness
    float startAlpha = 1.f, endAlpha = 0.f;
    bool lit = false;
    bool additive = true;   // (else see-through: sorted back to front on the GPU)
    int maxParticles = 500;
};

// A decal (Decal, collectDecals): its box and what it does to the surfaces in it (render/decals.h picks which are drawn)
struct DecalInfo {
    Mat4 toBox;                 // world -> its box's space (-0.5..0.5 each way; it projects down its -y)
    Vec3 center, down;          // the box's middle; its direction, in the world
    float radius = 1.f;         // round the box
    Vec3 color{1, 1, 1};        // linear
    float opacity = 1.f, roughness = -1.f, wetness = 0.f, angleFade = 0.5f;
    int texture = -1;
    int normalTexture = -1;
    float normalStrength = 1.f;
    float emissive = 0.f;       // (its colour as light, times this)
};

struct ProbeInfo {
    Vec3 position, boxMin, boxMax;
    int resolution = 128;
    bool boxProjection = true;
    uint64_t key = 0;   // what it's a picture of: its entity, place, size, resolution and the bake count
};

// A terrain's foliage to draw (collectPlants, render/scene_draw.h): its scattered instances in the terrain's space
// (Terrain::Plant: position, yaw, scale, shade), and ranges of them (a layer in a chunk) with what to draw them as
struct PlantSet {
    uint64_t key = 0;               // what the instances are: a new key when they change (uploaded again)
    const float* plants = nullptr;  // 6 floats each
    size_t count = 0;
    Mat4 model;                     // the terrain's
    struct Range {
        int mesh = -1;
        uint32_t first = 0, count = 0;
        Vec3 lo, hi;                // where the plants stand (terrain space)
        Vec3 color{1, 1, 1};        // sRGB
        float wind = 0.f, fade = 60.f, maxScale = 1.f;
    };
    std::vector<Range> ranges;
};

struct SceneRenderInfo {
    const std::vector<DrawItem>* items = nullptr;
    // Other views of the same scene (the editor's game view, split-screen players), up to Renderer::kMaxViews: each
    // is rendered whole (scene, ambient occlusion, bloom, the composite) into its own image, which Dear ImGui shows
    // (Renderer::viewTexture: ask for it, at the size given here, while building the UI). Their camera and look
    // fields are used, viewportW / H are the size (X / Y aren't); they share this view's shadows (the sun's cascades
    // follow this view's camera) and its lights (every light, not clustered), and are culled on the CPU.
    const std::vector<SceneRenderInfo>* views = nullptr;
    const std::vector<PlantSet>* plants = nullptr;   // foliage (main view only: no shadows, not in probes)
    const std::vector<LightInfo>* lights = nullptr;   // no directional light: a default sun is used
    const std::vector<Mat4>* joints = nullptr;        // skinning matrices of every skinned item (see DrawItem)
    const std::vector<ParticleDraw>* particles = nullptr;
    Mat4 viewProj;
    Vec3 camPos;
    Vec3 camForward{0, 0, -1};   // the shadow map covers the area in front of the camera
    // Look (Scene::render): sky / fog colour (sRGB), fog, ambient light, exposure and bloom
    Vec3 sky{0.62f, 0.75f, 0.90f};       // horizon / fog
    Vec3 zenith{0.36f, 0.56f, 0.88f};
    Vec3 ground{0.34f, 0.35f, 0.31f};
    float fovY = 60.f * DEG2RAD;           // the camera's (for the sky and the shadow cascades)
    float aspect = 16.f / 9.f;
    float fogDensity = 0.006f, ambient = 0.45f, exposure = 1.f, bloomStrength = 0.06f, bloomThreshold = 1.f;
    float aoStrength = 0.6f, aoRadius = 0.6f;   // ambient occlusion (screen space): how dark, how far (m); 0: off
    // Global illumination (RenderSettings::gi): a probe grid around this view's camera, a few redrawn per frame
    bool gi = false;
    float giSpacing = 3.f, giIntensity = 1.f;
    bool reflections = false;   // screen-space reflections (from the last frame's picture)
    bool ssgi = false;          // screen-space global illumination (the same picture), and its strength
    float ssgiIntensity = 1.f;
    float shadowSoftness = 1.f;   // the sun's shadows (RenderSettings::shadowSoftness)
    bool contactShadows = false;  // (RenderSettings'; the half-size traces')
    float waterTime = 0.f;        // the waves' clock (Physics::time() while playing: what's drawn matches what floats)
    bool volumetricFog = false;   // RenderSettings::volumetricFog and its settings
    float fogLight = 1.f, fogAnisotropy = 0.5f, fogHeightFalloff = 0.f;
    int tonemapper = 0;           // 0 PBR Neutral, 1 filmic
    float contrast = 1.f, saturation = 1.f, vignette = 0.f;
    bool autoExposure = false;        // (RenderSettings': the exposure follows the view's brightness)
    float exposureMin = -4.f, exposureMax = 4.f, adaptationSpeed = 1.f, localToneMapping = 0.f;
    float lensDirt = 0.f;             // (RenderSettings': how much the bloom lights up the lens's dirt)
    float lightShafts = 0.f;          // (RenderSettings': the sun's rays on screen, without volumetric fog)
    float depthOfField = 0.f, focusDistance = 10.f, motionBlur = 0.f;   // (RenderSettings'; the focus: its target's)
    float frameSeconds = 1.f / 60.f;  // (auto-exposure's easing: the time since the last frame)
    // The physically based sky (RenderSettings'; render/atmosphere.h): the sky colours above, the sun's colour (and
    // with a time of day, its direction) from the air instead
    bool physicalSky = false;
    float timeOfDay = -1.f, latitude = 40.f, haze = 1.f, aerialPerspective = 10.f;
    float cloudCover = 0.f;   // (RenderSettings')
    // A colour-blindness filter over the picture (the player's settings.colorBlind, Game::colorFilter()): 0 none,
    // 1 protanopia, 2 deuteranopia, 3 tritanopia. Daltonized: what that eye can't tell apart is moved into
    // differences it can see.
    int colorFilter = 0;
    float zNear = 0.1f, zFar = 2000.f;          // the camera's clip planes (to read depth back as distance)
    // A sky image (render/environment.h): drawn as the sky, lighting and reflected (null: the colours above)
    const Environment* environment = nullptr;
    float skyIntensity = 1.f, skyRotation = 0.f;   // brightness, turn around the vertical (radians)
    const std::vector<ProbeInfo>* probes = nullptr;   // the four nearest the camera are used
    const std::vector<DecalInfo>* decals = nullptr;   // the nearest Renderer::kMaxDecals are drawn (main view's camera)
    const std::vector<GpuEmitterInfo>* gpuEmitters = nullptr;   // simulated on the GPU (main view; null: none run)
    bool gpuEmitting = true;   // (they emit: false keeps what's flying)
    // Weather (RenderSettings'): wetness, puddles and rain
    float wetness = 0.f, puddles = 0.5f, rain = 0.f;
    int antialiasing = -1;   // a Renderer::Antialiasing to switch to (-1: keep the current one)
    float viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0;   // in window pixels
};

struct RenderStats {
    uint32_t drawCalls = 0;    // instanced draws in the main pass
    uint32_t triangles = 0;
    uint32_t objects = 0;      // drawn in the main pass (after culling)
    uint32_t culled = 0;       // outside the view
    uint32_t shadowCasters = 0;
    uint32_t shadowCulled = 0;       // sun cascades' casters skipped: their shadows land on nothing in view
    uint32_t shadowViewsDrawn = 0;   // shadow maps drawn this frame (the rest kept from before: Renderer's cache)
    uint64_t textureBytes = 0;    // textures on the GPU (streaming keeps it under its budget)
    uint64_t textureBudget = 0;
};

class Renderer {
public:
    // Edges: 4x MSAA (the default), temporal (jittered frames blended over time: softer, cheaper on detailed
    // scenes, handles shader aliasing too; needs a depth buffer it can read) or none
    enum class Antialiasing { Msaa, Temporal, Off };
    void setAntialiasing(Antialiasing mode);   // (rebuilds what depends on it: waits for the GPU)
    Antialiasing antialiasing() const { return aa_; }
    bool supports(Antialiasing mode) const;

    // The platform window must exist (platform::createWindow) before init().
    // The libraries must outlive the renderer; meshes and textures added to them later are uploaded automatically.
    void init(uint32_t width, uint32_t height, const MeshLibrary& meshes, const TextureLibrary& textures);
    // Which GPU init() picks when there are several: "discrete" (the default: the fastest), "integrated" (less power:
    // an editor on a laptop), or part of a GPU's name. VKWORLD_GPU overrides it. Set before init().
    void setGpuPreference(const std::string& preference) { gpuPreference_ = preference; }
    // A shipping game: no validation layer whatever VKWORLD_VALIDATION says (set before init())
    void setValidationAllowed(bool allowed) { validationAllowed_ = allowed; }
    // Where compiled pipelines are kept between runs (the driver's own cache: later starts skip compiling them);
    // "": none. Set before init(); saved by shutdown(). A cache from another GPU or driver is ignored.
    void setPipelineCachePath(const std::filesystem::path& file) { pipelineCachePath_ = file; }
    void initImGui();   // call after ImGui::CreateContext()
    void shutdown();

    void resize(uint32_t width, uint32_t height);
    void render(const SceneRenderInfo& info, ImDrawData* ui);

    // Saves the next rendered frame (UI included) as a PNG; check the result with screenshotDone()
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; screenshotResult_ = 0; }
    int screenshotDone() const { return screenshotResult_; }   // 0 pending / none, 1 saved, -1 failed

    const std::string& gpuName() const { return gpuName_; }
    // More frames would still change the picture with nothing else changing (global illumination's probes being
    // drawn the first time): an editor that only draws when something happens keeps drawing until then
    bool settling() const { return giUnmade_ > 0; }
    const RenderStats& stats() const { return stats_; }
    static int validationErrors();   // errors the validation layer has reported (VKWORLD_VALIDATION=1)
    // A TextureLibrary texture for Dear ImGui (AddImage); 0 until it has been uploaded (the next render() does it)
    ImTextureID uiTexture(int id) const;   // a library texture for Dear ImGui (made on first use)
    // Extra view `view`'s image (SceneRenderInfo::views), for Dear ImGui: made or resized to width x height now
    // (resizing waits for the GPU), drawn by the next render(). 0 if the view number or size can't be.
    static constexpr int kMaxViews = 4;
    ImTextureID viewTexture(int view, uint32_t width, uint32_t height);

    // HDR output (render/display.h): on a display that takes it, HDR10 (else scRGB); the scene's highlights are kept
    // up to the display's peak (VKWORLD_HDR_PEAK nits, 1000) with UI and paper white at VKWORLD_HDR_PAPER_WHITE
    // (200). The UI is drawn into a layer of its own then, blended in by the composite. Screenshots are SDR.
    void setHdrOutput(bool on);
    bool hdrSupported() const { return display::hdrAvailable(surfaceFormats_); }
    bool hdrActive() const { return output_ != display::Output::Sdr; }
    float hdrPeak() const { return hdrPeak_; }
    // Quality: what's drawn at all, for slower GPUs. High: everything the scene asks for, the sun's shadows 160 m
    // out; Medium: shadows 100 m, lights' shadow tiles half the size; Low: shadows 60 m, no screen-space reflections
    // or ambient occlusion. (VKWORLD_QUALITY=low|medium|high; games: settings.quality)
    enum class Quality { Low, Medium, High };
    void setQuality(Quality q) { quality_ = q; }
    Quality quality() const { return quality_; }   // (nits: the display's, VKWORLD_HDR_PEAK's, or 1000)
    // Render scale: the 3D scene drawn at this fraction of the viewport's width and height (0.5..1: a quarter to all
    // the pixels), brought up to full size by temporal upscaling (with temporal AA: this frame's few pixels and the
    // full-size history; otherwise a sharpened bilinear upscale); the UI stays at full size. Dynamic: the scale
    // follows the GPU's frame time to hold `targetFps` (between 0.5 and the set scale, in steps of 5 %).
    // VKWORLD_RENDER_SCALE overrides the set scale. Changing whether it upscales at all waits for the GPU.
    void setRenderScale(float scale);
    void setDynamicResolution(bool on, float targetFps = 60.f);
    float renderScale() const { return scaleNow_; }   // (this frame's: the dynamic one's choice)
    float gpuFrameMs() const { return gpuFrameMs_; }   // the last measured frame's GPU time (0: no timestamps)
    // The dynamic choice (plain: tested): the next scale from the current one, the GPU's frame time and the budget
    static float nextDynamicScale(float now, float gpuMs, float budgetMs, float maxScale);
    // Cached shadows' schedule (plain: tested): whether the sun's cascade `c` may be drawn again on frame `frame`
    // (the near two every frame, the third every second, the fourth every fourth, never both on one frame)
    static bool cascadeDue(uint64_t frame, uint32_t c);

private:
    static constexpr int MAX_FRAMES = 2;

    struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct PipelineDesc {
        const char* vertShader;
        const char* fragShader;   // nullptr: depth only (the shadow pass)
        bool depthTest, depthWrite, colorWrite;
        VkCullModeFlags cull;
        bool stencilTest;
        VkCompareOp stencilCompare;
        VkStencilOp stencilPass;
        uint32_t stencilWriteMask;
        uint32_t vertexAttributes = 3;   // position, normal, uv: only as many as the vertex shader reads
        bool blend = false;              // premultiplied see-through (colour only: alpha is the outline marker)
        bool shadow = false;             // into a shadow map (depth only), even with a fragment shader
        uint32_t attributeMask = 0;      // which of position / normal / uv (bits 0-2) instead (0: vertexAttributes)
        bool noColor = false;            // the scene's depth (and stencil) only: the depth prepass's own rendering
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;   // (the colour attachment's, if not the HDR image's)
        VkFormat colorFormat2 = VK_FORMAT_UNDEFINED;  // a second one's (the prepass's outputs)
        bool lean = false;               // scene.frag without material layers (its kLayers specialization: false)
        bool tint = false;               // the sun's shadow tint (RGBA16F, no depth): colour multiplied, alpha the nearest
    };

    void createInstance();
    void pickDevice();
    void createDevice();
    void createSwapchain();
    void destroySwapchainResources();
    void recreateSwapchain();
    void createPipelines();
    VkPipeline createPipeline(const PipelineDesc& desc);
    void createResources(const MeshLibrary& meshes);
    void uploadMeshes();   // (re)creates the vertex/index buffers from meshes_
    // Uploads textures added to textures_ since last time: all of them now, or (recorded into a frame's commands)
    // as many as fit this frame's budget
    void uploadTextures(VkCommandBuffer frame = VK_NULL_HANDLE);
    struct GpuTexture {
        Image image;                             // image.view reads it as sRGB colour (albedo, emissive)
        VkImageView linear = VK_NULL_HANDLE;     // the same bytes as plain numbers (normal and metal/rough maps, UI)
        uint32_t revision = 0;                   // the Texture's when uploaded (a newer one: uploaded again)
        bool nearest = false;                    // sampled with nearestSampler_
        // Streaming (texture_streaming.h): the image holds the full chain's levels from `first` down
        uint32_t first = 0, mips = 1;            // (mips: the full chain's; 1 without mipmaps)
        mutable bool pinned = false;             // always whole: no mipmaps, shown by the UI, a terrain's layer
        uint32_t wanted = ~0u;                   // the finest level reported lately (~0: none)
        double lastWanted = -1e9;
        uint32_t blockBytes = 0;                 // block compressed on the GPU: bytes a 4 x 4 block (0: RGBA8)
    };
    void reuploadChangedTextures();
    // Texture streaming (renderer_textures.cpp): the shaders' reports (binding 19, one buffer per frame in flight),
    // read back when the frame's buffer comes round again; textures remade with more or fewer levels a few at a
    // time, the old images kept until the frames using them are done
    bool streamingSupported_ = false, streaming_ = true;
    bool bcSupported_ = false;   // the GPU samples BC1 / BC3 / BC7 (cooked textures upload as they are)
    streaming::Settings streamSettings_;
    Buffer feedback_[MAX_FRAMES];
    std::vector<GpuTexture> retiredTextures_[MAX_FRAMES];
    std::vector<int> streamLead_;            // per texture: the one reported for it (its material's main map), or -1
    uint64_t residentTextureBytes_ = 0;
    uint32_t materialSetsMade_ = 0;          // (streaming leaves stale ones in the pool: emptied when it's nearly full)
    void ensureFeedback(size_t textures);    // (frame_'s buffer: as many entries as textures, set 0 binding 19)
    void streamTextures(VkCommandBuffer cb);
    // The sky image (renderer_environment.cpp)
    Image createFloatImage(const std::vector<const Environment::Level*>& levels);
    void createEnvironmentResources();
    void destroyEnvironmentResources();
    void writeEnvironmentDescriptors();
    void uploadEnvironment(const Environment* env);   // (when it's another one: waits for the GPU)
    void fillFrameUBO(const SceneRenderInfo& info, render_detail::FrameUBO& u, bool shadows);
    // GPU culling (renderer_cull.cpp): the main pass's solid objects culled by a compute shader into indirect draws
    bool gpuCulling_ = true;          // (VKWORLD_GPU_CULLING=0: on the CPU, as before)
    bool gpuCulled_ = false;          // this frame's scene batches are drawn indirectly
    uint32_t cullCount_[MAX_FRAMES] = {}, cullBatches_[MAX_FRAMES] = {};   // what each frame slot's pass culled
    Buffer cullBounds_[MAX_FRAMES], cullCommands_[MAX_FRAMES], instanceIndex_[MAX_FRAMES], cullParams_[MAX_FRAMES];
    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout cullLayout_ = VK_NULL_HANDLE;
    VkPipeline cullPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool cullPool_ = VK_NULL_HANDLE;
    VkDescriptorSet cullSets_[MAX_FRAMES] = {};
    uint32_t gpuVisible_ = 0, gpuTriangles_ = 0;   // from a frame before (read back after its fence)
    void createCulling();
    void destroyCulling();
    void writeCullSet();   // this frame's: its bounds, commands and instance indices
    void ensureCullCapacity(size_t objects, size_t batches);
    void prepareGpuCulling(const std::vector<DrawItem>& items, const std::vector<uint32_t>& visible,
                           const SceneRenderInfo& info);
    void recordCulling(VkCommandBuffer cb, const SceneRenderInfo& info);
    void readCullStats();
    // Occlusion culling: a depth pyramid (farthest depth per texel, halving), made from the main pass's depth at the
    // end of each frame and tested against by the next frame's culling (VKWORLD_OCCLUSION=0: off)
    bool occlusion_ = true;
    Image hiz_;                          // R32F, all levels (sampled by cull.comp)
    std::vector<VkImageView> hizLevels_; // one per level (written, and read by the next level)
    uint32_t hizWidth_ = 0, hizHeight_ = 0, hizMips_ = 0;
    VkImageView hizDepthView_ = VK_NULL_HANDLE;   // the depth view its first level was made from (remade with it)
    bool hizValid_ = false;              // holds a frame's depth (else: no occlusion culling)
    bool hizLayoutReady_ = false;        // (GENERAL)
    Mat4 hizViewProj_;                   // the camera it was made with
    VkDescriptorSetLayout hizSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout hizLayout_ = VK_NULL_HANDLE;
    VkPipeline hizDepthPipeline_ = VK_NULL_HANDLE, hizDepthMsPipeline_ = VK_NULL_HANDLE, hizReducePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool hizPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> hizSets_;   // level k's: its source (depth or level k - 1) and itself
    VkSampler hizSampler_ = VK_NULL_HANDLE;
    void createHiz();
    void destroyHiz();
    void ensureHiz(const SceneRenderInfo& info);   // sized to the viewport (waits for the GPU when it changes)
    void recordHiz(VkCommandBuffer cb, const SceneRenderInfo& info);
    // Anti-aliasing (renderer_taa.cpp): the pipelines that depend on the sample count, and the temporal pass
    void createSampledPipelines();
    void destroySampledPipelines();
    struct TaaState {   // one view's temporal anti-aliasing (the main view's, and each extra view's)
        Image img[2];                // the blended result, alternately written and read back as history
        VkDescriptorSet sets[2] = {};
        uint32_t index = 0;          // written this frame
        bool history = false;        // the other one holds last frame's
        bool fresh = false;          // just made: not yet in a readable layout
        Mat4 prevViewProj, reproject;
        float viewport[4] = {};
        float jitterPx[2] = {};      // this frame's jitter, in the drawn picture's pixels
    };
    void createTaaImages(TaaState& t);    // (the main view's with the post images, when temporal; views' on use)
    void destroyTaaImages(TaaState& t);
    void recordTaa(VkCommandBuffer cb, const SceneRenderInfo& info, TaaState& t, float scale = 1.f);
    // This frame's jitter for a view (into `u`), and its reprojection from last frame
    // (`display`: the viewport the history is kept at, when the scene is drawn smaller: render scale)
    void jitterView(const SceneRenderInfo& info, render_detail::FrameUBO& u, TaaState& t, const SceneRenderInfo* display = nullptr);
    Antialiasing aa_ = Antialiasing::Msaa;
    // Render scale: the set one, the dynamic one's state, this frame's
    float renderScaleSet_ = 1.f, scaleNow_ = 1.f, gpuFrameMs_ = 0.f, targetFps_ = 60.f;
    bool dynamicResolution_ = false;
    int scaleHold_ = 0;   // (frames before the dynamic scale may change again)
    bool upscaling() const { return renderScaleSet_ < 0.999f || dynamicResolution_; }
    VkSampleCountFlagBits maxSamples_ = VK_SAMPLE_COUNT_1_BIT;
    TaaState taa_;
    VkPipelineLayout taaLayout_ = VK_NULL_HANDLE;
    VkPipeline taaPipeline_ = VK_NULL_HANDLE;
    VkPipeline taaUpscalePipeline_ = VK_NULL_HANDLE;   // (upscaling alone: taa.frag without the history)
    uint32_t jitterIndex_ = 0;
    // Reflection probes (renderer_probes.cpp): baked on first sight, the nearest four bound (set 0, bindings 9-12)
    struct BakedProbe {
        uint64_t key = 0;
        Image radiance;
        float sh[9][3] = {};
        uint64_t lastUsed = 0;
        atmosphere::SkyState sky;   // (the sun, clouds and haze it was baked under: rebaked when they've moved enough)
    };
    std::vector<BakedProbe> probes_;
    std::vector<int> activeProbes_;           // indices into probes_, nearest first (at most 4)
    std::vector<ProbeInfo> activeProbeInfo_;
    uint64_t frameCounter_ = 0;
    void updateProbes(const SceneRenderInfo& info);   // bakes what's new (waits for the GPU), picks the nearest four
    bool bakeProbe(const SceneRenderInfo& info, const ProbeInfo& probe, BakedProbe& out);
    void writeProbeDescriptors();             // this frame's set: the active probes (black where none)
    void setProbeData(render_detail::FrameUBO& u);
    void destroyProbes();   // textures reloaded in place (TextureLibrary revisions): waits for the GPU
    // Set 1 for a draw: albedo, normal, metal/rough and emissive maps (defaults where it has none), made on first use
    VkDescriptorSet materialSet(const DrawItem& item);
    // Records the upload into `frame` when given (its staging buffer retired with that frame), else uploads now
    // `first`: the full chain's finest level to hold (texture streaming: smaller, the levels below it)
    GpuTexture createTexture(const uint8_t* rgba, uint32_t width, uint32_t height, VkCommandBuffer frame = VK_NULL_HANDLE,
                             bool mipmaps = true, uint32_t first = 0);
    // A library texture: from its rgba as above, or its cooked levels as they are (decoded first without BC support)
    GpuTexture createTexture(const Texture& tex, VkCommandBuffer frame, uint32_t first);
    GpuTexture createCookedTexture(const Texture& tex, VkCommandBuffer frame, uint32_t first);
    void destroyTexture(GpuTexture& t);
    VkCommandBuffer beginOneTimeCommands();
    void endOneTimeCommands(VkCommandBuffer cb);
    void createShadowResources();   // the samplers (and the shadow maps)
    // Culls and batches the draw items into this frame's instance buffer (after its fence has been waited on)
    void prepareInstances(const SceneRenderInfo& info, bool gpuCulling = true);
    void ensureIndexCapacity(size_t count);   // this frame's instance index list
    void uploadJoints(const SceneRenderInfo& info);   // this frame's joint matrices into its storage buffer
    void uploadParticles(const SceneRenderInfo& info);   // sorted: see-through ones back to front, then additive
    uint32_t particleAlpha_ = 0, particleAdditive_ = 0;  // this frame's counts
    VkPipeline createParticlePipeline(bool additive);
    // Shadows (renderer_shadows.cpp): 4 cascades for the sun (orthographic, each a slice of the view, snapped to
    // texels), up to 4 spot lights (perspective) and 2 point lights (six faces each, a cube). Layers 0-3 / 4-7 /
    // 8-19 of one matrix table in the frame data.
    struct ShadowArray {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView array = VK_NULL_HANDLE;    // sampled
        std::vector<VkImageView> layers;       // rendered into
        uint32_t size = 0, count = 0;
    };
    ShadowArray createShadowArray(uint32_t size, uint32_t layers, VkFormat color = VK_FORMAT_UNDEFINED);   // (a colour one: sampled, drawn into)
    void destroyShadowArray(ShadowArray& a);
    // Fills the frame's shadow matrices and cascade splits, and places the lights' shadows in the atlas (the
    // most important first, bigger); returns which views to render (bit per view). shadowLayer[i] gets the view
    // of lights[i]'s shadow (a point light's first face), or -1.
    uint64_t computeShadows(const SceneRenderInfo& info, Vec3 sunDir, bool sunShadows,
                            const std::vector<const LightInfo*>& lights, render_detail::FrameUBO& u,
                            std::vector<int>& shadowLayer);
    void recordShadows(VkCommandBuffer cb);
    // GPU pass times (core/profiler.h): timestamps between the passes, read back a couple of frames later
    static constexpr uint32_t kMaxTimestamps = 16;
    VkQueryPool timestamps_[MAX_FRAMES] = {};
    std::vector<const char*> stampNames_[MAX_FRAMES];
    uint32_t timestampBits_ = 0;
    float timestampPeriod_ = 1.f;   // ns per tick
    void timestamp(VkCommandBuffer cb, const char* name);
    void readGpuTimes();
    // (`scene`: `info` with the viewport the scene is drawn into: smaller with a render scale under 1)
    void recordCommands(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, const SceneRenderInfo& scene,
                        ImDrawData* ui);

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr);
    Buffer createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage);   // GPU memory, not mapped
    void destroyBuffer(Buffer& b);
    Image createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);   // screen-sized
    Image createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width,
                      uint32_t height, VkSampleCountFlagBits samples, uint32_t mipLevels, VkImageCreateFlags flags = 0);
    void destroyImage(Image& img);
    VkImageView createView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels = 1);
    VkShaderModule loadShader(const std::string& name);

    uint32_t width_ = 0, height_ = 0;
    bool resized_ = false;
    std::string gpuName_, gpuPreference_;
    bool validationAllowed_ = true;
    std::filesystem::path pipelineCachePath_;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    void createPipelineCache();
    void savePipelineCache();
    RenderStats stats_;
    const MeshLibrary* meshes_ = nullptr;
    uint32_t uploadedMeshVersion_ = 0;
    uint64_t uploadedRewriteSerial_ = 0;   // MeshLibrary::rewritten entries copied so far
    const TextureLibrary* textures_ = nullptr;
    std::vector<GpuTexture> gpuTextures_;   // same ids as the TextureLibrary
    GpuTexture whiteTexture_;               // for untextured draws
    GpuTexture flatNormal_;                 // the normal map of a flat surface
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool materialPool_ = VK_NULL_HANDLE;
    // Material sets by their six texture ids (colour, normal, metal / rough, emissive, height, detail; +1, 0: none)
    struct MaterialKey {
        uint16_t ids[6] = {};
        bool operator==(const MaterialKey& o) const { return std::equal(ids, ids + 6, o.ids); }
        bool uses(uint16_t id) const { return std::find(ids, ids + 6, id) != ids + 6; }
    };
    struct MaterialKeyHash {
        size_t operator()(const MaterialKey& k) const {
            uint64_t h = 1469598103934665603ull;
            for (uint16_t id : k.ids) h = (h ^ id) * 1099511628211ull;
            return (size_t)h;
        }
    };
    std::unordered_map<MaterialKey, VkDescriptorSet, MaterialKeyHash> materialSets_;
    std::unordered_map<MaterialKey, VkDescriptorSet, MaterialKeyHash> terrainSets_;   // terrains': four colour layers
    mutable std::unordered_map<int, VkDescriptorSet> uiSets_;      // texture id -> ImGui's set for it
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler nearestSampler_ = VK_NULL_HANDLE;
    Image envSky_, envRadiance_;
    Image fx_;         // the picture with the camera effects, copied back over hdr_ (camera_fx.glsl)
    Image fxTiles_;    // each 8 x 8 tile's largest near circle of confusion (fxSet_'s binding 3)
    bool fxTilesFresh_ = false;
    Mat4 fxPrevViewProj_;   // (the main view's last frame: the sky's motion)
    VkDescriptorSet fxSet_ = VK_NULL_HANDLE;   // (the picture, the depth, the motion vectors)
    Image shafts_;     // the sun's light shafts this frame, half size (shafts.glsl); the composite's binding 7
    Vec3 shaftsSunDir_{0, -1, 0}, shaftsSunColor_{0, 0, 0};   // (the main view's sun this frame: fillFrameUBO)
    Image lensDirt_;   // smudges, specks and streaks on the lens, made once (renderer_post.cpp); the composite's binding 6
    void createLensDirt();
    void writeLensDirtBinding();
    VkSampler envSampler_ = VK_NULL_HANDLE;
    uint32_t envId_ = 0;
    float envSh_[9][3] = {};   // textures imported with "filter": "nearest"
    float maxAnisotropy_ = 1.f;             // 1 = anisotropic filtering not available

    static constexpr uint32_t kCascadeSize = 2048, kCascades = 4, kAtlasSize = 4096;
    static constexpr uint32_t kShadowLayers = 64;     // render_detail::kShadowViews: cascades, then atlas views
    static constexpr uint32_t kViewCascades = 48;     // ... the last 16: the extra views' cascades (4 each)
    static constexpr float kShadowDistance = 160.f;   // the sun's shadows reach this far from the camera
    VkFormat shadowFormat_ = VK_FORMAT_D32_SFLOAT;
    ShadowArray cascades_, atlas_;   // the sun's cascades; spot and point lights' tiles in one image
    // The sun's shadow tint (a layer per cascade, half its size; set 0 binding 38): rgb what see-through and thin
    // translucent casters let through (multiplied), a the nearest one's depth (white, 1: nothing)
    ShadowArray shadowTint_;
    bool shadowTintFresh_ = false;
    bool tinted_[4] = {};            // (each cascade's tint layer holds something: frame.shadowParams.w)
    VkPipeline shadowTintPipeline_ = VK_NULL_HANDLE;
    struct ShadowTile {
        uint32_t x = 0, y = 0, size = 0;
    };
    ShadowTile shadowTiles_[kShadowLayers];   // atlas views: where in the atlas
    // The extra views' own cascades (layers kViewCascades + 4 * view, atlas tiles): placed this frame, their splits
    bool viewShadowed_[kMaxViews] = {};
    float viewSplits_[kMaxViews][4] = {};
    bool shadowMapReady_ = false;   // in SHADER_READ_ONLY layout
    uint64_t shadowLayers_ = 0;     // this frame's shadow views to render (bits 0-3 cascades, then the atlas's)
    // Cached shadows: each layer keeps what it was last drawn with (its camera, its place in the atlas, a hash of its
    // casters: meshes, levels, transforms, vertices rewritten), and is only drawn again when one of them changes; the
    // farther cascades are brought up to date every second / fourth frame (their old cameras kept meanwhile).
    // VKWORLD_SHADOW_CACHE=0: every layer every frame, as before.
    struct ShadowCache {
        Mat4 viewProj;
        uint64_t signature = 0;
        ShadowTile tile;
        float scale[4] = {};
        bool valid = false, empty = false;
    };
    ShadowCache shadowCache_[kShadowLayers];
    uint64_t shadowSignature_[kShadowLayers] = {};   // (this frame's, from prepareInstances)
    uint64_t shadowDeferred_ = 0;    // layers kept from an earlier frame by the cascades' schedule
    bool shadowCacheOn_ = true;
    uint32_t shadowsDrawn_ = 0;      // layers drawn this frame (stats)
    // Lights (binding 14) and their clusters (binding 15), per frame
    Buffer lights_[MAX_FRAMES], clusters_[MAX_FRAMES];
    void createLightBuffers();
    float viewNear_ = 0.1f, viewFar_ = 1000.f;   // what's in view, along it (fitViewDepth)
    bool fittedCascades_ = true;                 // the sun's cascades from the nearest thing in view (VKWORLD_SHADOW_FIT=0: from the camera)
    bool leanShading_ = true;                    // materials without layers drawn with the lean lit pass (VKWORLD_LEAN_SHADING=0: all layered)
    bool casterCulling_ = true;                  // the sun's casters only if their shadows reach the view (VKWORLD_SHADOW_CULL=0: all)
    float viewFloor_ = -1e9f;                    // the lowest thing in the scene (fitViewDepth; shadows below land on nothing)
    Vec3 shadowSunDir_{0, -1, 0};                // this frame's sun, as it shines (computeShadows)
    void fitViewDepth(const SceneRenderInfo& info);
    // Decals (renderer_decals.cpp): the nearest, a buffer per frame (binding 24), their textures (binding 25)
public:
    static constexpr uint32_t kMaxDecals = 256, kDecalTextures = 16, kDecalNormals = 8;
private:
    static constexpr size_t kMaxDecalIndices = 16384;   // (decals listed in the clusters, all of them together)
    Buffer decalBuffers_[MAX_FRAMES];
    Buffer decalClusters_[MAX_FRAMES];   // (binding 27: as the lights' clusters, binding 15)
    ClusterSetup clusterSetup_;          // the main view's clusters this frame (writeLights)
    bool clusterSetupValid_ = false;
    void createDecalBuffers();
    void readyFreshImages(VkCommandBuffer cb);   // images just made: into the layouts they're read in
    void writeDecalStandIns();   // (every frame's set: white textures and flat normal maps until a frame picks its own)
    void writeDecals(const SceneRenderInfo& info, render_detail::FrameUBO& u);
    // Foliage (renderer_lights.cpp): every terrain's plants in one buffer per frame (binding 16), rewritten when
    // they change; drawn instanced after the solid objects
    Buffer plantBuffers_[MAX_FRAMES];
    uint64_t plantKeys_[MAX_FRAMES] = {};
    std::vector<uint32_t> plantBases_;   // this frame: where each set's plants start
    VkPipeline foliagePipeline_ = VK_NULL_HANDLE;
    void uploadPlants(const SceneRenderInfo& info);
    void drawPlants(VkCommandBuffer cb, const SceneRenderInfo& info, VkPipeline pipeline = VK_NULL_HANDLE);
    uint32_t lightCount_ = 0, clusteredIndices_ = 0;   // (stats)
    // Spot lights' cookies: images projected through the cone, in a texture array (binding 6)
    static constexpr uint32_t kCookieSize = 256, kCookieLayers = 16;
    Image cookies_;
    bool cookiesReady_ = false;
    std::unordered_map<std::string, int> cookieLayers_;   // path -> layer (-1: couldn't load)
    void createCookies();
    void updateCookies(const SceneRenderInfo& info);
    // The lights buffer, and (clustered: the main view) their clusters; probe views loop over every light
    void writeLights(const SceneRenderInfo& info, const std::vector<const LightInfo*>& lights,
                     const std::vector<int>& shadowView, render_detail::FrameUBO& u, bool clustered);
    bool frameShadows_ = false;     // the sun casts shadows this frame
    VkSampler shadowSampler_ = VK_NULL_HANDLE;   // (nearest: raw depths)
    VkSampler shadowCompareSampler_ = VK_NULL_HANDLE;   // (comparing, bilinear: filtered shadow lookups)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowCutoutPipeline_ = VK_NULL_HANDLE;   // with a fragment shader dropping the holes

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderDone_;   // one per swapchain image
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    Image colorMsaa_, depth_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline scenePipeline_ = VK_NULL_HANDLE;     // lit objects
    // The depth prepass (opaque things' depth first: then each pixel is lit once), and the lit pipelines after it
    // (early depth tests, no depth writes); VKWORLD_PREPASS=0 turns it off
    VkPipeline prepassPipeline_ = VK_NULL_HANDLE, prepassFoliagePipeline_ = VK_NULL_HANDLE;
    VkPipeline sceneEarlyPipeline_ = VK_NULL_HANDLE, foliageEarlyPipeline_ = VK_NULL_HANDLE;
    VkPipeline sceneLayeredPipeline_ = VK_NULL_HANDLE, sceneEarlyLayeredPipeline_ = VK_NULL_HANDLE;   // (with layers)
    bool prepass_ = true;
    // Volumetric fog (renderer_fog.cpp): per frame in flight, the cells' in-scattered light and the volume summed
    // from the camera out (scene.frag / sky.frag's binding 21)
    struct FogImages { Image scatter, volume; };
    FogImages fog_[MAX_FRAMES];
    VkDescriptorSetLayout fogSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fogLayout_ = VK_NULL_HANDLE;
    VkPipeline fogInjectPipeline_ = VK_NULL_HANDLE, fogIntegratePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool fogPool_ = VK_NULL_HANDLE;
    VkDescriptorSet fogSets_[MAX_FRAMES] = {};
    VkSampler fogSampler_ = VK_NULL_HANDLE;
    // Each cell column's surface distance (fog_surface.comp, from this frame's depth after the prepass; binding 22)
    Image fogSurface_[MAX_FRAMES];
    VkDescriptorSetLayout fogSurfaceSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fogSurfaceLayout_ = VK_NULL_HANDLE;
    VkPipeline fogSurfacePipeline_ = VK_NULL_HANDLE, fogSurfaceMsPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet fogSurfaceSets_[MAX_FRAMES] = {};
    VkImageView fogSurfaceDepth_[MAX_FRAMES] = {};   // (the depth view each set was written with)
    bool fogOn_ = false;      // this frame draws it
    float fogFar_ = 200.f;    // its last slice's end: the far end of what's in view (as the light clusters')
    // Last frame's fog (its frame data, for the cells' history); fogHistory_: whether the other frame's volume holds it
    Mat4 fogPrevViewProj_;
    Vec3 fogPrevCam_, fogPrevForward_;
    float fogPrevRange_[2] = {0.f, 0.f};
    bool fogHistory_ = false;
    float fogNear_ = 0.1f;    // ... and the near end (the slices start at half of it: from high up, little air is
                              // in the way, and the slices are thinner where the light is)
    Image createVolume();
    void createFog();
    void destroyFog();
    void recordFog(VkCommandBuffer cb, const SceneRenderInfo& info, bool readDepth, VkRect2D area);
    Quality quality_ = Quality::High;
    float shadowDistance() const { return quality_ == Quality::High ? kShadowDistance : quality_ == Quality::Medium ? 100.f : 60.f; }
    bool reflectionsOn(const SceneRenderInfo& info) const { return info.reflections && quality_ != Quality::Low; }
    bool ssgiOn(const SceneRenderInfo& info) const {
        return info.ssgi && info.ssgiIntensity > 0.f && quality_ != Quality::Low;
    }
    // The last frame's picture is captured for either
    bool captureOn(const SceneRenderInfo& info) const { return reflectionsOn(info) || ssgiOn(info); }
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;   // lit see-through objects (no depth writes)
    VkPipeline waterPipeline_ = VK_NULL_HANDLE;         // ... water: both sides (its underside, seen from below)
    VkPipeline stencilPipeline_ = VK_NULL_HANDLE;   // marks the selected object's silhouette in stencil
    VkPipeline outlinePipeline_ = VK_NULL_HANDLE;   // extruded hull drawn where stencil != 1
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSets_[MAX_FRAMES] = {};

    Buffer vertexBuffer_, indexBuffer_, ubo_[MAX_FRAMES];
    // Instance data (renderer_instances.cpp): one per draw item (then the outlined ones again), kept on the GPU
    // between frames; only what changed since the last upload is copied up, from this frame's staging buffer at the
    // start of whatever command buffer comes next (recordInstanceUpload). Batches list them by index (binding 13).
    Buffer instanceStore_;                  // device-local (binding 2)
    uint64_t instanceStoreGen_ = 1, storeBound_[MAX_FRAMES] = {};   // which store each frame's set points to
    Buffer instanceStaging_[MAX_FRAMES];
    std::vector<uint8_t> instanceMirror_;   // what instanceStore_ holds
    std::vector<VkBufferCopy> pendingCopies_;
    uint32_t uploadedInstances_ = 0;        // (stats: rewritten this frame)
    uint32_t listedInstances_ = 0;          // entries in this frame's index list (GPU LOD regions follow them)
    void ensureInstanceStore(size_t count);
    void recordInstanceUpload(VkCommandBuffer cb);
    Buffer joints_[MAX_FRAMES];      // skinning matrices (storage buffer)
    Buffer particles_[MAX_FRAMES];   // particles to draw (storage buffer)
    // GPU particles (renderer_particles.cpp): a ring of slots per emitter in one state buffer, written to the draw
    // list after the CPU's particles by a compute pass, drawn indirectly
    struct GpuRing {
        uint32_t key, offset, slots, head;
        float pending;   // (fractional particles owed by the rate)
    };
    std::vector<std::pair<uint32_t, uint32_t>> gpuLayout_;   // (key, slots) as laid out
    std::vector<GpuRing> gpuRings_;
    uint32_t gpuTotalSlots_ = 0, gpuParticleSlots_ = 0, gpuEmitterCount_ = 0;
    bool gpuClearState_ = false, gpuSplash_ = false;
    float gpuDt_ = 0.f;
    Buffer gpuParticleState_, gpuEmitterBuffers_[MAX_FRAMES], gpuIndirect_[MAX_FRAMES];
    Buffer gpuUnsorted_, gpuKeys_;   // (the see-through ones before their sort, and their keys)
    uint32_t gpuSortSize_ = 0;       // (the keys: a power of two)
    bool gpuAnySeeThrough_ = false;
    VkPipeline gpuSortPipeline_ = VK_NULL_HANDLE, gpuGatherPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout gpuParticleSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout gpuParticleLayout_ = VK_NULL_HANDLE;
    VkPipeline gpuParticlePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool gpuParticlePool_ = VK_NULL_HANDLE;
    VkDescriptorSet gpuParticleSets_[MAX_FRAMES] = {};
    void createGpuParticles();
    void destroyGpuParticles();
    void prepareGpuParticles(const SceneRenderInfo& info);
    void recordGpuParticles(VkCommandBuffer cb);
    VkPipeline particleAlphaPipeline_ = VK_NULL_HANDLE, particleAdditivePipeline_ = VK_NULL_HANDLE;
    Buffer skinBuffer_;
    size_t vertexCapacity_ = 0, indexCapacity_ = 0, uploadedVertices_ = 0, uploadedIndices_ = 0;
    std::vector<Buffer> retired_[MAX_FRAMES];   // staging buffers to free once that frame's fence has passed              // SkinVertex per vertex (vertex buffer, binding 1)
    // Consecutive instances drawn with one call: same mesh and texture
    struct Batch {
        int mesh = 0;
        VkDescriptorSet texture = VK_NULL_HANDLE;
        uint32_t first = 0, count = 0;   // its entries in the instance index list
        int lod = 0;                     // which level of the mesh (assets/mesh_lod.h; 0: the mesh itself)
        bool water = false;              // (see-through: water, drawn from both sides: waterPipeline_)
        bool layered = false;            // its material has layers: the lit pass's full variant (sceneLayeredPipeline_)
    };
    // Opaque batches (and plants) (`culled`: the GPU culling's indirect draws for the main view's batches): their
    // depth (the prepass, in a rendering of its own), or lit (after it; or depth tested and written, without it)
    void drawOpaque(VkCommandBuffer cb, const SceneRenderInfo& info, const std::vector<Batch>& batches, bool culled,
                    bool depthOnly);
    // The prepass's rendering: depth (and stencil) cleared and filled in the scene's rectangle; the lit pass loads it
    void recordPrepass(VkCommandBuffer cb, const SceneRenderInfo& info, const std::vector<Batch>& batches, bool culled,
                       VkRect2D area);
    MeshLod rangeOf(const Batch& b) const;   // the indices to draw for it
    void drawBatch(VkCommandBuffer cb, const Batch& b);
    std::vector<Batch> sceneBatches_, outlineBatches_, transparentBatches_;   // (transparent: back to front)
    std::vector<Batch> viewBatches_[kMaxViews], viewTransparent_[kMaxViews];   // the extra views' (culled on the CPU)
    VkDeviceSize uboStride_ = 0;   // the frame UBO's slots (this view, then the extra views), aligned
    struct ViewImage {   // an extra view's picture (swapchain format), sampled by Dear ImGui
        Image image;
        VkDescriptorSet ui = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        bool fresh = false;   // made, never drawn: cleared to be readable before the UI samples it
        TaaState taa;         // (images made when it's first drawn with temporal AA; sets from the start)
    };
    ViewImage viewImages_[kMaxViews];
    void destroyViewImage(ViewImage& v);
    void recordView(VkCommandBuffer cb, int view, const SceneRenderInfo& info);

    // Dynamic GI (renderer_gi.cpp): a grid of probes around the camera, their slots wrapping around as it moves (a
    // probe keeps its slot while it stays in the grid); a few redrawn each frame (six small faces into an atlas)
    // and projected onto spherical harmonics by gi_project.comp into giProbes_ (set 0, binding 17)
    static constexpr int kGiX = 8, kGiY = 4, kGiZ = 8, kGiProbes = kGiX * kGiY * kGiZ;
    static constexpr int kGiPerFrame = 2, kGiFace = 32;
    static constexpr int kUboSlots = 1 + kMaxViews + 6 * kGiPerFrame;   // this view, the extra views, probe faces
    struct GiSlot {
        int cell[3] = {0, 0, 0};   // the world cell it holds
        bool made = false;         // drawn at least once for that cell
        uint64_t drawn = 0;        // the frame it was last drawn
    };
    GiSlot giSlots_[kGiProbes];
    Buffer giProbes_;                  // per slot: position (w = 1: made), then nine SH coefficients (vec4 each)
    Buffer giDispatch_[MAX_FRAMES];    // gi_project.comp's parameters
    Image giAtlas_, giAtlasMsaa_, giDepth_;
    VkDescriptorSetLayout giSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout giLayout_ = VK_NULL_HANDLE;
    VkPipeline giPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool giPool_ = VK_NULL_HANDLE;
    VkDescriptorSet giSets_[MAX_FRAMES] = {};
    int giCount_ = 0;                  // probes drawn this frame
    int giDrawSlot_[kGiPerFrame] = {};
    Vec3 giDrawPos_[kGiPerFrame];
    std::vector<Batch> giBatches_[kGiPerFrame];
    uint64_t giFrame_ = 0;
    int giUnmade_ = 0;
    bool giOn_ = false;                // (this frame)
    VkSampleCountFlagBits giSamples_ = VK_SAMPLE_COUNT_1_BIT;   // (the atlas's, remade when MSAA changes)
    void createGi();
    void destroyGi();
    // Before this view's frame data (their lights are written to the shared buffer first, this view's last): the
    // grid around its camera, which probes to draw, their faces' frame data. finishGi() then gives `u` the grid and
    // the faces `u`'s shadows and lights, into the UBO's slots.
    void planGi(const SceneRenderInfo& info);
    void finishGi(render_detail::FrameUBO& u);
    std::vector<uint8_t> giFaces_;     // (the faces' FrameUBOs, finished by finishGi; bytes: the type is internal)
    float giGrid_[3][4] = {};          // (this frame's giOrigin, giDims, giCell)
    void recordGi(VkCommandBuffer cb, const SceneRenderInfo& info);
    std::vector<Batch> shadowBatches_[kShadowLayers];         // per shadow layer: solid casters ...
    std::vector<Batch> shadowCutoutBatches_[kShadowLayers];   // ... and ones with holes (cut-outs, see-through)
    std::vector<Batch> shadowTintBatches_[kCascades];         // ... and the sun's ones letting light through, tinted
    Mat4 shadowViewProj_[kShadowLayers];
    VkPipeline skyPipeline_ = VK_NULL_HANDLE;
    VkPipeline createSkyPipeline();
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_[MAX_FRAMES] = {};
    VkSemaphore imageAvailable_[MAX_FRAMES] = {};
    VkFence inFlight_[MAX_FRAMES] = {};
    uint32_t frame_ = 0;
    bool imguiInitialized_ = false;

    // ---- HDR and post-processing: the scene renders into hdr_ (linear, floating point), then bloom goes down and
    // back up bloom_'s mip chain, and the composite pass tonemaps both onto the swapchain image
    static constexpr uint32_t kMaxBloomMips = 6;
    VkFormat hdrFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    Image hdr_, bloom_;
    // Ambient occlusion: half-size, from the scene's depth (ssao.frag), darkening the composite
    Image ao_;
    VkExtent2D aoSize_{};
    VkImageView depthSampleView_ = VK_NULL_HANDLE;   // depth_'s depth, for reading
    bool aoSupported_ = false;                       // the depth format can be sampled
    VkDescriptorSet aoSet_ = VK_NULL_HANDLE;
    VkPipeline aoPipeline_ = VK_NULL_HANDLE;
    VkPipeline cameraFxPipeline_ = VK_NULL_HANDLE;   // depth of field and motion blur (camera_fx.glsl): fxSet_
    VkPipeline fxTilesPipeline_ = VK_NULL_HANDLE;    // (its near field's tiles: fxTiles_)
    VkPipeline shaftsPipeline_ = VK_NULL_HANDLE;   // light shafts (shafts.glsl): half size, over the depth (aoSet_)
    // Screen-space reflections: this frame's picture and distances captured after the post passes (prevColor_, set
    // 0 binding 18), for scene.frag's rays next frame; the camera it was taken with
    // (mipped by blits, where the format allows: rough reflections and the GI's bounces read blurrier levels)
    Image prevColor_;
    static constexpr uint32_t kPrevMips = 6;
    uint32_t prevMips_ = 1;
    VkImageView prevMip0_ = VK_NULL_HANDLE;   // (its first level: what the capture draws into)
    bool prevColorFresh_ = false;   // (made: readable before anything samples it)
    // Water's refraction (renderer.cpp): with water in the main view, the scene pass stops after the opaque things
    // and the sky, the capture copies the picture and its distances here (set 0, binding 26), and goes on with the
    // see-through things; water reads what's behind it there
    Image refraction_;
    // Motion vectors (renderer_instances.cpp: prepareMotion): with temporal AA, the depth prepass also writes each
    // pixel's motion since last frame (velocity_, RG16F, uv units; kNoVelocity where nothing was drawn), from each
    // object's last model matrix and joints (kept by DrawItem::key) and last frame's unjittered camera
    static constexpr float kNoVelocity = 30000.f;
    Image velocity_;
    bool velocityFresh_ = false;
    bool velocityOn_ = false;        // this frame's prepass writes it
    VkPipeline prepassVelocityPipeline_ = VK_NULL_HANDLE, prepassFoliageVelocityPipeline_ = VK_NULL_HANDLE;
    struct Motion {
        Mat4 model;
        std::vector<Mat4> joints;
        uint64_t frame = 0;          // (seen in: older ones are dropped)
    };
    std::unordered_map<uint32_t, Motion> motion_;
    std::vector<Mat4> prevModels_;       // per draw item this frame: its model matrix last frame
    std::vector<int> prevJointOffsets_;  // ... its joints last frame, in the joints buffer after this frame's (-1: none)
    std::vector<Mat4> prevJoints_;
    Mat4 motionPrevViewProj_;
    bool motionPrevValid_ = false;
    uint64_t motionFrame_ = 0;
    bool velocityDrawn_ = false;     // this frame's prepass wrote them (the temporal pass reads them)
    void prepareMotion(const SceneRenderInfo& info);
    // Reflections and GI traced at half size (renderer_trace.cpp): without MSAA the prepass also writes each pixel's
    // surface (gbuffer_: normal, roughness), and before the lit pass ssr_trace.frag / gi_trace.frag trace one pixel
    // of each 2 x 2 block (a different one each frame) into traced_ / tracedLight_ (two each: this frame's, set 0
    // bindings 31 / 32, and last frame's, its history, 33 / 34, by frame_), with the traced pixel's distance
    // (tracedDepth_, 35) for scene.frag's upsample; set 0 binding 29 the depth, 30 the surface, 36 the motion
    Image gbuffer_, traced_[2], tracedLight_[2], tracedDepth_;
    VkPipeline ssrTracePipeline_ = VK_NULL_HANDLE, giTracePipeline_ = VK_NULL_HANDLE;
    VkPipeline ssrTraceMsPipeline_ = VK_NULL_HANDLE, giTraceMsPipeline_ = VK_NULL_HANDLE;
    Image velocityMs_, gbufferMs_;   // (with MSAA: the prepass's outputs, multisampled, resolved into velocity_, gbuffer_)
    bool traceOn_ = false;           // this frame's main view traces at half size
    bool traceHistory_ = false;      // last frame did (its results are this frame's history)
    uint32_t traceIndex_ = 0;
    void createTraceImages();
    void writeTraceBindings();
    void recordTrace(VkCommandBuffer cb, const SceneRenderInfo& scene);
    // Ambient occlusion before lighting (renderer_trace.cpp): the main view's, after the prepass, into aoLit_[frame_]
    // (frame i's set 0 binding 37: its own; the other is its history), which scene.frag puts on the indirect light
    // alone, turned to its bent normal; the composite then leaves it out
    Image aoLit_[2];
    VkDescriptorSet aoTemporalSets_[2] = {};   // (frame i's: the depth, the motion, aoLit_[1 - i])
    VkPipeline aoTemporalPipeline_ = VK_NULL_HANDLE;
    bool aoHistory_ = false;         // last frame's aoLit_ is good (not just made)
    Mat4 aoViewProj_;                // (the camera it was made from: without motion vectors, used only if it's still)
    bool aoLitOn_ = false;           // this frame's main view does it
    bool aoLitWanted_ = true;        // (VKWORLD_AO_LIT=0: over the finished picture, as before)
    void recordAoLit(VkCommandBuffer cb, const SceneRenderInfo& scene);
    bool refractionFresh_ = false;
    bool refractOn_ = false;        // this frame's main view does it
    bool viewRefracts_ = false, viewRefract_[kMaxViews] = {};   // (the extra views': in recordView)
    void writeRefractionBinding();
    void copyOpaque(VkCommandBuffer cb, float zNear, float zFar, float vx, float vy);
    VkDescriptorSet ssrSet_ = VK_NULL_HANDLE;
    VkPipeline ssrCapturePipeline_ = VK_NULL_HANDLE;
    bool ssrValid_ = false;         // last frame was captured (reflections on, the same picture size)
    Mat4 ssrViewProj_;
    Vec3 ssrCamPos_, ssrCamForward_;
    float ssrViewport_[4] = {};
    void writeSsrBinding();
    // The display (createSwapchain): the 8-bit format the UI and extra views are drawn in, what's presented
    std::vector<VkSurfaceFormatKHR> surfaceFormats_;
    VkFormat sdrFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    display::Output output_ = display::Output::Sdr;
    bool hdrWanted_ = false;
    bool uiLayered_ = false;        // the UI is drawn into uiLayer_ and blended in by the composite (HDR, or the test switch)
    bool uiLayerForced_ = false;    // (VKWORLD_UI_LAYER=1: the layered path on an SDR display, to test it)
    float hdrPeak_ = 1000.f, hdrPaperWhite_ = 200.f;
    bool hdrPeakForced_ = false;          // (VKWORLD_HDR_PEAK: not the display's own)
    bool hdrMetadataSupported_ = false;   // VK_EXT_hdr_metadata
    display::Luminance displayLuminance_;   // what the display says it can do (0s: it doesn't)
    void describeHdr();
    Image uiLayer_;
    bool uiLayerFresh_ = false;
    // Auto-exposure and local tone mapping (renderer_post.cpp): the view's log2 luminance, mipped down to its mean
    // (luminance.frag, then blits), and the exposure eased towards it (adapt.frag into exposure_[0], copied to [1]
    // as next frame's starting point)
    static constexpr uint32_t kLumWidth = 256, kLumHeight = 144, kLumMips = 9;
    Image lum_, exposure_[2];
    VkImageView lumMip0_ = VK_NULL_HANDLE;
    VkDescriptorSet lumSet_ = VK_NULL_HANDLE, adaptSet_ = VK_NULL_HANDLE;
    VkPipeline lumPipeline_ = VK_NULL_HANDLE, adaptPipeline_ = VK_NULL_HANDLE;
    bool lumSupported_ = false;     // (the luminance format can be blitted with filtering)
    VkSampler lumSampler_ = VK_NULL_HANDLE;   // (bilinear, every mip)
    bool lumFresh_ = false;         // (made: readable before anything samples it)
    bool exposureHistory_ = false;  // last frame adapted (else: start at the target)
    // The physically based sky (renderer_sky.cpp): its transmittance and sky tables (the latter: set 0, binding 23),
    // and the CPU's sums for the frame's colours, kept while the sun and haze stay
    static constexpr uint32_t kSkyTransmittanceW = 256, kSkyTransmittanceH = 64, kSkyViewW = 192, kSkyViewH = 108;
    static constexpr uint32_t kSkyMultipleSize = 32;
    Image skyTransmittance_, skyView_, skyMultiple_;
    VkPipeline skyTransmittancePipeline_ = VK_NULL_HANDLE, skyViewPipeline_ = VK_NULL_HANDLE;
    VkPipeline skyMultiplePipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet skyViewSet_ = VK_NULL_HANDLE;   // (the sky pass's: the transmittance table)
    bool skyFresh_ = false;     // (made: readable before anything samples them)
    bool skyOn_ = false;        // this frame's main view has it
    bool skyViewMade_ = false;
    float skyHaze_ = -1.f;      // the transmittance table's (-1: not drawn)
    float skyHazeNow_ = 1.f, skyLightHaze_ = -1.f;
    float cloudCoverFor_ = 0.f, cloudFraction_ = 0.f;   // (the cover the clouds' average was found for)
    Vec3 skyToSun_{0, 1, 0}, skyViewSun_{0, 1, 0}, skyLightSun_{0, 0, 0}, skySunHere_{1, 1, 1};
    atmosphere::SkyLight skyLight_;
    Vec3 skyHarmonics_[9];
    void createSky();
    void destroySky();
    void recordSky(VkCommandBuffer cb);
    void applyAtmosphere(const SceneRenderInfo& info, LightInfo& sun, Vec3& horizon, Vec3& zenith, Vec3& ground,
                         render_detail::FrameUBO& u);
    VkPipeline compositeViewPipeline_ = VK_NULL_HANDLE;   // (into the extra views' images: sdrFormat_)
    uint32_t bloomMips_ = 0;
    VkImageView bloomViews_[kMaxBloomMips] = {};
    VkExtent2D bloomSizes_[kMaxBloomMips] = {};
    VkSampler postSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout postLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool postPool_ = VK_NULL_HANDLE;
    VkDescriptorSet prefilterSet_ = VK_NULL_HANDLE, compositeSet_ = VK_NULL_HANDLE;
    VkDescriptorSet downSets_[kMaxBloomMips] = {}, upSets_[kMaxBloomMips] = {};
    VkPipeline bloomDownPipeline_ = VK_NULL_HANDLE, bloomUpPipeline_ = VK_NULL_HANDLE, compositePipeline_ = VK_NULL_HANDLE;
    VkPipeline createPostPipeline(const char* fragShader, VkFormat format, bool additive, VkPipelineLayout layout,
                                  VkFormat format2);
    VkPipeline createPostPipeline(const char* fragShader, VkFormat format, bool additive,
                                  VkPipelineLayout layout = VK_NULL_HANDLE);   // (null: postLayout_)
    void createPostImages();    // window-sized: made again when the swapchain is
    void destroyPostImages();
    // Bloom, AO and the composite: onto swapchain image `imageIndex`, or (view >= 0) into that extra view's image
    void recordPost(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, int view = -1);

    std::string screenshotPath_;   // (".hdr": the scene's light before tone mapping, as a Radiance file)
    float screenshotExposure_ = 1.f;
    bool hdrShotRecorded_ = false;   // (this frame copied the HDR image out)
    int screenshotResult_ = 0;
    bool canReadSwapchain_ = false;   // swapchain images allow transfers out
    Buffer readback_;
    void saveScreenshot();
};
