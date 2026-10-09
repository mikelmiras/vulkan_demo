// Vulkan 1.3 renderer: scene (MSAA + depth/stencil), selection outline, and the ImGui overlay.
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "assets/meshes.h"
#include "core/vecmath.h"

struct ImDrawData;

// One mesh to draw. The renderer knows nothing about the scene: callers flatten it into these.
struct DrawItem {
    Mat4 model;
    int mesh = 0;               // id in the MeshLibrary
    Vec3 color{1, 1, 1};        // sRGB
    bool checker = false;       // procedural checkerboard + grid
    bool outlined = false;      // draw the selection outline around it
};

struct SceneRenderInfo {
    const std::vector<DrawItem>* items = nullptr;
    Mat4 viewProj;
    Vec3 camPos;
    float viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0;   // in window pixels
};

struct RenderStats {
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
};

class Renderer {
public:
    // The platform window must exist (platform::createWindow) before init().
    // `meshes` must outlive the renderer; meshes added to it later are uploaded automatically.
    void init(uint32_t width, uint32_t height, const MeshLibrary& meshes);
    void initImGui();   // call after ImGui::CreateContext()
    void shutdown();

    void resize(uint32_t width, uint32_t height);
    void render(const SceneRenderInfo& info, ImDrawData* ui);

    // Saves the next rendered frame (UI included) as a PNG; check the result with screenshotDone()
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; screenshotResult_ = 0; }
    int screenshotDone() const { return screenshotResult_; }   // 0 pending / none, 1 saved, -1 failed

    const std::string& gpuName() const { return gpuName_; }
    const RenderStats& stats() const { return stats_; }

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
        const char* fragShader;
        bool depthTest, depthWrite, colorWrite;
        VkCullModeFlags cull;
        bool stencilTest;
        VkCompareOp stencilCompare;
        VkStencilOp stencilPass;
        uint32_t stencilWriteMask;
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
    void recordCommands(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, ImDrawData* ui);

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr);
    void destroyBuffer(Buffer& b);
    Image createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroyImage(Image& img);
    VkImageView createView(VkImage image, VkFormat format, VkImageAspectFlags aspect);
    VkShaderModule loadShader(const std::string& name);

    uint32_t width_ = 0, height_ = 0;
    bool resized_ = false;
    std::string gpuName_;
    RenderStats stats_;
    const MeshLibrary* meshes_ = nullptr;
    uint32_t uploadedMeshVersion_ = 0;

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
    VkPipeline stencilPipeline_ = VK_NULL_HANDLE;   // marks the selected object's silhouette in stencil
    VkPipeline outlinePipeline_ = VK_NULL_HANDLE;   // extruded hull drawn where stencil != 1
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSets_[MAX_FRAMES] = {};

    Buffer vertexBuffer_, indexBuffer_, ubo_[MAX_FRAMES];
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_[MAX_FRAMES] = {};
    VkSemaphore imageAvailable_[MAX_FRAMES] = {};
    VkFence inFlight_[MAX_FRAMES] = {};
    uint32_t frame_ = 0;
    bool imguiInitialized_ = false;

    std::string screenshotPath_;
    int screenshotResult_ = 0;
    bool canReadSwapchain_ = false;   // swapchain images allow transfers out
    Buffer readback_;
    void saveScreenshot();
};
