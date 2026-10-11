// Vulkan 1.3 renderer: scene (MSAA + depth/stencil), selection outline, and the ImGui overlay.
#pragma once

#include <vulkan/vulkan.h>

#include "imgui.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "assets/meshes.h"
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
    ImTextureID uiTexture(int id) const;

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
    void uploadTextures(); // uploads textures added to textures_ since last time
    struct GpuTexture {
        Image image;                             // image.view reads it as sRGB colour (albedo, emissive)
        VkImageView linear = VK_NULL_HANDLE;     // the same bytes as plain numbers (normal and metal/rough maps, UI)
        VkDescriptorSet set = VK_NULL_HANDLE;    // the linear view alone, for Dear ImGui
    };
    // Set 1 for a draw: albedo, normal, metal/rough and emissive maps (defaults where it has none), made on first use
    VkDescriptorSet materialSet(const DrawItem& item);
    GpuTexture createTexture(const uint8_t* rgba, uint32_t width, uint32_t height);
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
    // texels), and up to 4 spot lights (perspective). Layers 0-3 / 4-7 of one matrix table in the frame data.
    struct ShadowArray {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView array = VK_NULL_HANDLE;    // sampled
        VkImageView layers[4] = {};            // rendered into
        uint32_t size = 0;
    };
    ShadowArray createShadowArray(uint32_t size);
    void destroyShadowArray(ShadowArray& a);
    // Fills the frame's shadow matrices and cascade splits; returns which layers to render (bit per layer)
    uint32_t computeShadows(const SceneRenderInfo& info, Vec3 sunDir, bool sunShadows,
                            const std::vector<const LightInfo*>& spots, render_detail::FrameUBO& u) const;
    void recordShadows(VkCommandBuffer cb);
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
    const TextureLibrary* textures_ = nullptr;
    std::vector<GpuTexture> gpuTextures_;   // same ids as the TextureLibrary
    GpuTexture whiteTexture_;               // for untextured draws
    GpuTexture flatNormal_;                 // the normal map of a flat surface
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool materialPool_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkDescriptorSet> materialSets_;   // by the four texture ids
    VkDescriptorSetLayout textureSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool texturePool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    float maxAnisotropy_ = 1.f;             // 1 = anisotropic filtering not available

    static constexpr uint32_t kCascadeSize = 2048, kSpotShadowSize = 1024, kCascades = 4, kSpotShadows = 4;
    static constexpr float kShadowDistance = 160.f;   // the sun's shadows reach this far from the camera
    VkFormat shadowFormat_ = VK_FORMAT_D32_SFLOAT;
    ShadowArray cascades_, spotShadows_;
    bool shadowMapReady_ = false;   // in SHADER_READ_ONLY layout
    uint32_t shadowLayers_ = 0;     // this frame's shadow layers to render (bits 0-3 cascades, 4-7 spots)
    bool frameShadows_ = false;     // the sun casts shadows this frame
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;

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
    Buffer skinBuffer_;              // SkinVertex per vertex (vertex buffer, binding 1)
    // Consecutive instances drawn with one call: same mesh and texture
    struct Batch {
        int mesh = 0;
        VkDescriptorSet texture = VK_NULL_HANDLE;
        uint32_t first = 0, count = 0;
    };
    std::vector<Batch> sceneBatches_, outlineBatches_, transparentBatches_;   // (transparent: back to front)
    std::vector<Batch> shadowBatches_[8];   // per shadow layer
    Mat4 shadowViewProj_[8];
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
    VkPipeline createPostPipeline(const char* fragShader, VkFormat format, bool additive);
    void createPostImages();    // window-sized: made again when the swapchain is
    void destroyPostImages();
    void recordPost(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info);

    std::string screenshotPath_;
    int screenshotResult_ = 0;
    bool canReadSwapchain_ = false;   // swapchain images allow transfers out
    Buffer readback_;
    void saveScreenshot();
};
