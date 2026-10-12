// Vulkan 1.3 renderer: scene (MSAA + depth/stencil), selection outline, and the ImGui overlay.
#pragma once

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/meshes.h"
#include "render/environment.h"
#include "assets/textures.h"
#include "core/vecmath.h"

struct ImDrawData;
namespace render_detail { struct FrameUBO; }   // render/renderer_internal.h

// One mesh to draw. The renderer knows nothing about the scene: callers flatten it into these.
struct DrawItem {
    Mat4 model;
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
    // A terrain (scene/terrain.h): its mesh's skin weights mix four layers, each a texture (sRGB) times a tint
    bool terrain = false;
    int layerTextures[4] = {-1, -1, -1, -1};
    int terrainLayers = -1;     // its layers' tints and tilings (a matrix, column per layer) in SceneRenderInfo::joints
};

// One particle to draw: a camera-facing dot
struct ParticleDraw {
    Vec3 position;
    float size = 0.1f;      // metres across
    float color[4];         // rgb linear (times its brightness), alpha
    bool additive = true;   // light adds up; else blended over what's behind
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
};

// A reflection probe to draw with (ReflectionProbe, scene/scene.h): baked when its key is new
struct ProbeInfo {
    Vec3 position, boxMin, boxMax;
    int resolution = 128;
    bool boxProjection = true;
    uint64_t key = 0;   // what it's a picture of: its entity, place, size, resolution and the bake count
};

struct SceneRenderInfo {
    const std::vector<DrawItem>* items = nullptr;
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
    float zNear = 0.1f, zFar = 2000.f;          // the camera's clip planes (to read depth back as distance)
    // A sky image (render/environment.h): drawn as the sky, lighting and reflected (null: the colours above)
    const Environment* environment = nullptr;
    float skyIntensity = 1.f, skyRotation = 0.f;   // brightness, turn around the vertical (radians)
    const std::vector<ProbeInfo>* probes = nullptr;   // the four nearest the camera are used
    int antialiasing = -1;   // a Renderer::Antialiasing to switch to (-1: keep the current one)
    float viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0;   // in window pixels
};

struct RenderStats {
    uint32_t drawCalls = 0;    // instanced draws in the main pass
    uint32_t triangles = 0;
    uint32_t objects = 0;      // drawn in the main pass (after culling)
    uint32_t culled = 0;       // outside the view
    uint32_t shadowCasters = 0;
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
    void initImGui();   // call after ImGui::CreateContext()
    void shutdown();

    void resize(uint32_t width, uint32_t height);
    void render(const SceneRenderInfo& info, ImDrawData* ui);

    // Saves the next rendered frame (UI included) as a PNG; check the result with screenshotDone()
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; screenshotResult_ = 0; }
    int screenshotDone() const { return screenshotResult_; }   // 0 pending / none, 1 saved, -1 failed

    const std::string& gpuName() const { return gpuName_; }
    const RenderStats& stats() const { return stats_; }
    static int validationErrors();   // errors the validation layer has reported (VKWORLD_VALIDATION=1)
    // A TextureLibrary texture for Dear ImGui (AddImage); 0 until it has been uploaded (the next render() does it)
    ImTextureID uiTexture(int id) const;   // a library texture for Dear ImGui (made on first use)

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
    };
    void reuploadChangedTextures();
    // The sky image (renderer_environment.cpp)
    Image createFloatImage(const std::vector<const Environment::Level*>& levels);
    void createEnvironmentResources();
    void destroyEnvironmentResources();
    void writeEnvironmentDescriptors();
    void uploadEnvironment(const Environment* env);   // (when it's another one: waits for the GPU)
    void fillFrameUBO(const SceneRenderInfo& info, render_detail::FrameUBO& u, bool shadows);
    // Anti-aliasing (renderer_taa.cpp): the pipelines that depend on the sample count, and the temporal pass
    void createSampledPipelines();
    void destroySampledPipelines();
    void createTaaImages();    // (with the post images, when temporal)
    void destroyTaaImages();
    void recordTaa(VkCommandBuffer cb, const SceneRenderInfo& info);
    Antialiasing aa_ = Antialiasing::Msaa;
    VkSampleCountFlagBits maxSamples_ = VK_SAMPLE_COUNT_1_BIT;
    Image taa_[2];                 // the blended result, alternately written and read back as history
    VkDescriptorSet taaSets_[2] = {};
    VkPipelineLayout taaLayout_ = VK_NULL_HANDLE;
    VkPipeline taaPipeline_ = VK_NULL_HANDLE;
    uint32_t taaIndex_ = 0;        // written this frame
    bool taaHistory_ = false;      // the other one holds last frame's
    bool taaFresh_ = false;        // just made: not yet in a readable layout
    uint32_t jitterIndex_ = 0;
    Mat4 prevViewProj_, taaReproject_;
    float taaViewport_[4] = {};
    // Reflection probes (renderer_probes.cpp): baked on first sight, the nearest four bound (set 0, bindings 9-12)
    struct BakedProbe {
        uint64_t key = 0;
        Image radiance;
        float sh[9][3] = {};
        uint64_t lastUsed = 0;
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
    GpuTexture createTexture(const uint8_t* rgba, uint32_t width, uint32_t height, VkCommandBuffer frame = VK_NULL_HANDLE,
                             bool mipmaps = true);
    void destroyTexture(GpuTexture& t);
    VkCommandBuffer beginOneTimeCommands();
    void endOneTimeCommands(VkCommandBuffer cb);
    void createShadowResources();   // the samplers (and the shadow maps)
    // Culls and batches the draw items into this frame's instance buffer (after its fence has been waited on)
    void prepareInstances(const SceneRenderInfo& info);
    void ensureInstanceCapacity(size_t count);
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
    ShadowArray createShadowArray(uint32_t size, uint32_t layers);
    void destroyShadowArray(ShadowArray& a);
    // Fills the frame's shadow matrices and cascade splits; returns which layers to render (bit per layer).
    // shadowLayer[i] gets the layer of lights[i]'s shadow (a point light's first face), or -1.
    uint32_t computeShadows(const SceneRenderInfo& info, Vec3 sunDir, bool sunShadows,
                            const std::vector<const LightInfo*>& lights, render_detail::FrameUBO& u,
                            std::vector<int>& shadowLayer) const;
    void recordShadows(VkCommandBuffer cb);
    // GPU pass times (core/profiler.h): timestamps between the passes, read back a couple of frames later
    static constexpr uint32_t kMaxTimestamps = 8;
    VkQueryPool timestamps_[MAX_FRAMES] = {};
    std::vector<const char*> stampNames_[MAX_FRAMES];
    uint32_t timestampBits_ = 0;
    float timestampPeriod_ = 1.f;   // ns per tick
    void timestamp(VkCommandBuffer cb, const char* name);
    void readGpuTimes();
    void recordCommands(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, ImDrawData* ui);

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr);
    void destroyBuffer(Buffer& b);
    Image createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);   // screen-sized
    Image createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t width,
                      uint32_t height, VkSampleCountFlagBits samples, uint32_t mipLevels, VkImageCreateFlags flags = 0);
    void destroyImage(Image& img);
    VkImageView createView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels = 1);
    VkShaderModule loadShader(const std::string& name);

    uint32_t width_ = 0, height_ = 0;
    bool resized_ = false;
    std::string gpuName_;
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
    std::unordered_map<uint64_t, VkDescriptorSet> materialSets_;   // by the four texture ids
    std::unordered_map<uint64_t, VkDescriptorSet> terrainSets_;    // terrains': four colour layers
    mutable std::unordered_map<int, VkDescriptorSet> uiSets_;      // texture id -> ImGui's set for it
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler nearestSampler_ = VK_NULL_HANDLE;
    Image envSky_, envRadiance_;
    VkSampler envSampler_ = VK_NULL_HANDLE;
    uint32_t envId_ = 0;
    float envSh_[9][3] = {};   // textures imported with "filter": "nearest"
    float maxAnisotropy_ = 1.f;             // 1 = anisotropic filtering not available

    static constexpr uint32_t kCascadeSize = 2048, kSpotShadowSize = 1024, kCascades = 4, kSpotShadows = 4;
    static constexpr uint32_t kPointShadowSize = 512, kPointShadows = 2, kShadowLayers = 20;
    static constexpr float kShadowDistance = 160.f;   // the sun's shadows reach this far from the camera
    VkFormat shadowFormat_ = VK_FORMAT_D32_SFLOAT;
    ShadowArray cascades_, spotShadows_, pointShadows_;
    bool shadowMapReady_ = false;   // in SHADER_READ_ONLY layout
    uint32_t shadowLayers_ = 0;     // this frame's shadow layers to render (bits 0-3 cascades, 4-7 spots, 8-19 points)
    bool frameShadows_ = false;     // the sun casts shadows this frame
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
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
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;   // lit see-through objects (no depth writes)
    VkPipeline stencilPipeline_ = VK_NULL_HANDLE;   // marks the selected object's silhouette in stencil
    VkPipeline outlinePipeline_ = VK_NULL_HANDLE;   // extruded hull drawn where stencil != 1
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSets_[MAX_FRAMES] = {};

    Buffer vertexBuffer_, indexBuffer_, ubo_[MAX_FRAMES];
    Buffer instances_[MAX_FRAMES];   // InstanceData for every object drawn in the frame (storage buffer)
    Buffer joints_[MAX_FRAMES];      // skinning matrices (storage buffer)
    Buffer particles_[MAX_FRAMES];   // particles to draw (storage buffer)
    VkPipeline particleAlphaPipeline_ = VK_NULL_HANDLE, particleAdditivePipeline_ = VK_NULL_HANDLE;
    Buffer skinBuffer_;
    size_t vertexCapacity_ = 0, indexCapacity_ = 0, uploadedVertices_ = 0, uploadedIndices_ = 0;
    std::vector<Buffer> retired_[MAX_FRAMES];   // staging buffers to free once that frame's fence has passed              // SkinVertex per vertex (vertex buffer, binding 1)
    // Consecutive instances drawn with one call: same mesh and texture
    struct Batch {
        int mesh = 0;
        VkDescriptorSet texture = VK_NULL_HANDLE;
        uint32_t first = 0, count = 0;
    };
    std::vector<Batch> sceneBatches_, outlineBatches_, transparentBatches_;   // (transparent: back to front)
    std::vector<Batch> shadowBatches_[kShadowLayers];         // per shadow layer: solid casters ...
    std::vector<Batch> shadowCutoutBatches_[kShadowLayers];   // ... and ones with holes (cut-outs, see-through)
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
    VkPipeline createPostPipeline(const char* fragShader, VkFormat format, bool additive,
                                  VkPipelineLayout layout = VK_NULL_HANDLE);   // (null: postLayout_)
    void createPostImages();    // window-sized: made again when the swapchain is
    void destroyPostImages();
    void recordPost(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info);

    std::string screenshotPath_;
    int screenshotResult_ = 0;
    bool canReadSwapchain_ = false;   // swapchain images allow transfers out
    Buffer readback_;
    void saveScreenshot();
};
