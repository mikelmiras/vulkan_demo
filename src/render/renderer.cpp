#include "render/renderer.h"

#include "core/png.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "platform/platform.h"

#define VK_CHECK(expr)                                                                         \
    do {                                                                                       \
        VkResult r_ = (expr);                                                                  \
        if (r_ != VK_SUCCESS) {                                                                \
            std::fprintf(stderr, "Vulkan error %d at %s:%d\n  %s\n", r_, __FILE__, __LINE__, #expr); \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)

[[noreturn]] static void fatal(const char* msg) {
    std::fprintf(stderr, "fatal: %s\n", msg);
    std::exit(1);
}

namespace {

// Must match shaders/common.glsl
constexpr int kMaxPointLights = 8;
struct FrameUBO {
    Mat4 viewProj;
    Mat4 lightViewProj;
    float camPos[4];
    float fogColor[4];   // rgb linear, w = density
    float viewport[4];   // width, height, outline width in pixels, unused
    float sunDir[4];     // xyz, w = 1 if it casts shadows
    float sunColor[4];   // rgb linear * intensity
    float ambient[4];    // rgb, w = point light count
    float pointPos[kMaxPointLights][4];     // xyz, w = range
    float pointColor[kMaxPointLights][4];   // rgb linear * intensity
};

struct PushConstants {
    Mat4 model;
    float color[4];    // rgb (linear albedo; sRGB for the outline)
    float params[4];   // x = checker ground, y = texture tiling, z = textured
};

void set4(float out[4], Vec3 v, float w) {
    out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = w;
}

constexpr Vec3 kSkySrgb{0.62f, 0.75f, 0.90f};
constexpr Vec3 kOutlineSrgb{1.0f, 0.55f, 0.1f};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[validation] %s\n", data->pMessage);
    return VK_FALSE;
}

void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                  VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void checkVk(VkResult r) {
    if (r != VK_SUCCESS) std::fprintf(stderr, "[imgui] Vulkan error %d\n", r);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------
void Renderer::init(uint32_t width, uint32_t height, const MeshLibrary& meshes, const TextureLibrary& textures) {
    width_ = width;
    height_ = height;
    meshes_ = &meshes;
    textures_ = &textures;

    createInstance();
    surface_ = platform::createVulkanSurface(instance_);
    if (!surface_) fatal("cannot create Vulkan window surface");
    pickDevice();
    createDevice();
    createSwapchain();
    createPipelines();
    createShadowResources();
    createResources(meshes);
}

void Renderer::createInstance() {
    bool validation = false;
#ifndef NDEBUG
    validation = true;
#endif
    if (const char* env = std::getenv("VKWORLD_VALIDATION")) validation = env[0] == '1';

    if (validation) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        validation = false;
        for (const auto& l : layers)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation = true;
        if (!validation) std::fprintf(stderr, "validation layer not available, continuing without it\n");
    }

    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, platform::vulkanSurfaceExtension()};
    std::vector<const char*> layers;
    if (validation) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Vulkan World Editor";
    appInfo.apiVersion = VK_API_VERSION_1_3;

#ifdef __APPLE__
    // MoltenVK / KosmicKrisp are non-conformant "portability" drivers that are only enumerated on request
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#ifdef __APPLE__
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (create) VK_CHECK(create(instance_, &mci, nullptr, &messenger_));
        std::printf("Validation layer enabled\n");
    }
}

void Renderer::pickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
        vkGetPhysicalDeviceFeatures2(d, &f2);
        if (!f13.dynamicRendering) continue;

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
        if (!hasSwapchain) continue;

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, qprops.data());
        int family = -1;
        for (uint32_t i = 0; i < qCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = (int)i; break; }
        }
        if (family < 0) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                 : 1;
        if (score > bestScore) {
            bestScore = score;
            gpu_ = d;
            queueFamily_ = (uint32_t)family;
        }
    }
    if (!gpu_) fatal("no suitable Vulkan 1.3 GPU found");

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    gpuName_ = props.deviceName;
    std::printf("Using GPU: %s\n", props.deviceName);

    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                                props.limits.framebufferStencilSampleCounts;
    samples_ = (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

    for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { depthFormat_ = f; break; }
    }
    if (depthFormat_ == VK_FORMAT_UNDEFINED) fatal("no depth/stencil format supported");
}

void Renderer::createDevice() {
    float priority = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(gpu_, &supported);
    if (supported.samplerAnisotropy) {   // sharper textures at grazing angles
        f2.features.samplerAnisotropy = VK_TRUE;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu_, &props);
        maxAnisotropy_ = std::min(props.limits.maxSamplerAnisotropy, 8.f);
    }

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Portability drivers (MoltenVK etc.) require VK_KHR_portability_subset to be enabled when they expose it
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &extCount, available.data());
    for (const auto& e : available)
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) exts.push_back("VK_KHR_portability_subset");

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    VK_CHECK(vkCreateDevice(gpu_, &ci, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
}

uint32_t Renderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    fatal("no suitable memory type");
}

// Host-visible, persistently mapped buffer (fine for a small demo; integrated GPUs share memory anyway)
Renderer::Buffer Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK_CHECK(vkCreateBuffer(device_, &ci, nullptr, &b.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));
    VK_CHECK(vkMapMemory(device_, b.memory, 0, size, 0, &b.mapped));
    if (data) std::memcpy(b.mapped, data, size);
    return b;
}

void Renderer::destroyBuffer(Buffer& b) {
    vkDestroyBuffer(device_, b.buffer, nullptr);
    vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

Renderer::Image Renderer::createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    return createImage(format, usage, aspect, extent_.width, extent_.height, samples_, 1);
}

Renderer::Image Renderer::createImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                      uint32_t width, uint32_t height, VkSampleCountFlagBits samples,
                                      uint32_t mipLevels) {
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VK_CHECK(vkCreateImage(device_, &ci, nullptr, &img.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(device_, img.image, img.memory, 0));
    img.view = createView(img.image, format, aspect, mipLevels);
    return img;
}

void Renderer::destroyImage(Image& img) {
    vkDestroyImageView(device_, img.view, nullptr);
    vkDestroyImage(device_, img.image, nullptr);
    vkFreeMemory(device_, img.memory, nullptr);
    img = {};
}

VkImageView Renderer::createView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    VkImageView view;
    VK_CHECK(vkCreateImageView(device_, &ci, nullptr, &view));
    return view;
}

void Renderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps));

    if (swapFormat_ == VK_FORMAT_UNDEFINED) {
        // UNORM target: shaders output gamma-encoded colour, which keeps ImGui's sRGB colours correct.
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &count, formats.data());
        swapFormat_ = formats[0].format;
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                swapFormat_ = f.format;
                break;
            }
    }

    if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(width_, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = swapFormat_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    canReadSwapchain_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (canReadSwapchain_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapImages_.data());
    for (VkImage img : swapImages_) swapViews_.push_back(createView(img, swapFormat_, VK_IMAGE_ASPECT_COLOR_BIT));

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderDone_.resize(count);
    for (auto& s : renderDone_) VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &s));

    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        colorMsaa_ = createImage(swapFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
    depth_ = createImage(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
}

void Renderer::destroySwapchainResources() {
    for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
    swapViews_.clear();
    renderDone_.clear();
    if (colorMsaa_.image) destroyImage(colorMsaa_);
    destroyImage(depth_);
}

void Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(device_);
    destroySwapchainResources();
    createSwapchain();
    resized_ = false;
}

void Renderer::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    resized_ = true;
}

VkShaderModule Renderer::loadShader(const std::string& name) {
    // Shaders are compiled into <build>/shaders. The executable is either next to that folder
    // (single-config generators) or one level below it (Visual Studio: <build>/Debug, <build>/Release).
    namespace fs = std::filesystem;
    const fs::path exeDir = platform::executableDir();
    fs::path path;
    for (const fs::path& dir : {exeDir / "shaders", exeDir.parent_path() / "shaders"}) {
        std::error_code ec;
        if (fs::exists(dir / name, ec)) { path = dir / name; break; }
    }
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (path.empty() || !f) {
        std::fprintf(stderr, "cannot find shader %s next to %s\n", name.c_str(), exeDir.u8string().c_str());
        std::exit(1);
    }
    size_t size = (size_t)f.tellg();
    std::vector<uint32_t> code((size + 3) / 4);
    f.seekg(0);
    f.read((char*)code.data(), (std::streamsize)size);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &mod));
    return mod;
}

VkPipeline Renderer::createPipeline(const PipelineDesc& d) {
    const bool depthOnly = d.fragShader == nullptr;   // the shadow pass
    VkShaderModule vs = loadShader(d.vertShader), fs = depthOnly ? VK_NULL_HANDLE : loadShader(d.fragShader);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };

    VkVertexInputBindingDescription vib{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vib;
    vi.vertexAttributeDescriptionCount = d.vertexAttributes;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    if (depthOnly) {   // slope-scaled bias against shadow acne
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 1.5f;
        rs.depthBiasSlopeFactor = 2.0f;
    }

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = depthOnly ? VK_SAMPLE_COUNT_1_BIT : samples_;

    VkStencilOpState st{};
    st.failOp = VK_STENCIL_OP_KEEP;
    st.passOp = d.stencilPass;
    st.depthFailOp = VK_STENCIL_OP_KEEP;
    st.compareOp = d.stencilCompare;
    st.compareMask = 0xff;
    st.writeMask = d.stencilWriteMask;
    st.reference = 1;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest;
    ds.depthWriteEnable = d.depthWrite;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.stencilTestEnable = d.stencilTest;
    ds.front = ds.back = st;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = d.colorWrite ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                                      : 0;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = depthOnly ? 0 : 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = depthOnly ? 0 : 1;
    rci.pColorAttachmentFormats = &swapFormat_;
    rci.depthAttachmentFormat = depthOnly ? shadowFormat_ : depthFormat_;
    rci.stencilAttachmentFormat = depthOnly ? VK_FORMAT_UNDEFINED : depthFormat_;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = depthOnly ? 1 : 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = pipelineLayout_;
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));

    vkDestroyShaderModule(device_, vs, nullptr);
    if (fs) vkDestroyShaderModule(device_, fs, nullptr);
    return pipeline;
}

void Renderer::createPipelines() {
    // Set 0, per frame: the frame data and the shadow map
    VkDescriptorSetLayoutBinding frameBindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = frameBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_));
    // Set 1, per draw: the material's texture
    VkDescriptorSetLayoutBinding texBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                            nullptr};
    dslci.bindingCount = 1;
    dslci.pBindings = &texBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &textureSetLayout_));

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkDescriptorSetLayout layouts[2] = {setLayout_, textureSetLayout_};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_));

    // Lit scene objects: depth tested, no stencil
    scenePipeline_ = createPipeline({"scene.vert.spv", "scene.frag.spv", true, true, true, VK_CULL_MODE_BACK_BIT,
                                     false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0});
    // Selected object's full silhouette into stencil (ignores depth so occluded parts are marked too)
    stencilPipeline_ = createPipeline({"scene.vert.spv", "outline.frag.spv", false, false, false, VK_CULL_MODE_NONE,
                                       true, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, 0xff});
    // Hull extruded in screen space, drawn only outside the silhouette -> outline visible through walls
    outlinePipeline_ = createPipeline({"outline.vert.spv", "outline.frag.spv", false, false, true, VK_CULL_MODE_NONE,
                                       true, VK_COMPARE_OP_NOT_EQUAL, VK_STENCIL_OP_KEEP, 0, 2});
    // Depth from the sun's point of view (no fragment shader)
    shadowPipeline_ = createPipeline({"shadow.vert.spv", nullptr, true, true, false, VK_CULL_MODE_NONE, false,
                                      VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0, 1});
}

// ---------------------------------------------------------------------------------------------
// Textures and the shadow map
// ---------------------------------------------------------------------------------------------
VkCommandBuffer Renderer::beginOneTimeCommands() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

void Renderer::endOneTimeCommands(VkCommandBuffer cb) {
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue_));
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cb);
}

void Renderer::createShadowResources() {
    shadowMap_ = createImage(shadowFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, kShadowSize, kShadowSize, VK_SAMPLE_COUNT_1_BIT, 1);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;   // the shader compares and filters itself
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &shadowSampler_));

    // Textures: trilinear, repeating, anisotropic where available
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.anisotropyEnable = maxAnisotropy_ > 1.f;
    sci.maxAnisotropy = maxAnisotropy_;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_));
}

// Uploads RGBA8 sRGB pixels to a device-local image with a full mip chain (when the format can be blitted)
Renderer::GpuTexture Renderer::createTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu_, format, &fp);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    uint32_t mips = 1;
    if ((fp.optimalTilingFeatures & blit) == blit)
        while ((std::max(width, height) >> mips) > 0) ++mips;

    GpuTexture t;
    t.image = createImage(format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, width, height, VK_SAMPLE_COUNT_1_BIT, mips);
    Buffer staging = createBuffer((VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba);

    auto barrier = [&](VkCommandBuffer cb, uint32_t mip, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                       VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, 1};
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    VkCommandBuffer cb = beginOneTimeCommands();
    barrier(cb, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int32_t w = (int32_t)width, h = (int32_t)height;
    for (uint32_t m = 1; m < mips; ++m) {   // each level is a filtered half-size copy of the previous one
        barrier(cb, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        region.srcOffsets[1] = {w, h, 1};
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        region.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cb, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        barrier(cb, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT);
    }
    barrier(cb, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    endOneTimeCommands(cb);
    destroyBuffer(staging);

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = texturePool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &textureSetLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &t.set));
    VkDescriptorImageInfo ii{sampler_, t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = t.set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
    return t;
}

void Renderer::destroyTexture(GpuTexture& t) {
    destroyImage(t.image);   // the descriptor set goes with its pool
    t = {};
}

void Renderer::uploadTextures() {
    while ((int)gpuTextures_.size() < textures_->count()) {
        const Texture& tex = textures_->get((int)gpuTextures_.size());
        gpuTextures_.push_back(createTexture(tex.rgba.data(), tex.width, tex.height));
    }
    // The shadow map starts out readable, so frames without shadows can still bind it
    if (!shadowMapReady_) {
        VkCommandBuffer cb = beginOneTimeCommands();
        imageBarrier(cb, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        endOneTimeCommands(cb);
        shadowMapReady_ = true;
    }
}

// Orthographic camera looking along the sun, covering a square area in front of the viewer, moved in whole
// shadow-map texels so the shadow edges don't shimmer as the camera moves
Mat4 Renderer::shadowMatrix(const SceneRenderInfo& info, Vec3 sunDir) const {
    const float r = kShadowRadius;
    Vec3 flat{info.camForward.x, 0.f, info.camForward.z};
    Vec3 center = info.camPos + (length(flat) > 1e-3f ? normalize(flat) * (r * 0.6f) : Vec3{});
    Vec3 up = std::fabs(sunDir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Mat4 view = lookAt(center - sunDir * (r * 3.f), center, up);
    Mat4 proj = orthographic(-r, r, -r, r, 0.1f, r * 6.f);
    Mat4 vp = proj * view;
    float half = kShadowSize * 0.5f;
    float ox = vp.m[12] * half, oy = vp.m[13] * half;   // where the world origin lands, in texels
    proj.m[12] += (std::round(ox) - ox) / half;
    proj.m[13] += (std::round(oy) - oy) / half;
    return proj * view;
}

void Renderer::uploadMeshes() {
    if (vertexBuffer_.buffer) {   // replacing: the old buffers may still be in use by frames in flight
        vkDeviceWaitIdle(device_);
        destroyBuffer(vertexBuffer_);
        destroyBuffer(indexBuffer_);
    }
    vertexBuffer_ = createBuffer(meshes_->vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 meshes_->vertices.data());
    indexBuffer_ = createBuffer(meshes_->indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                meshes_->indices.data());
    uploadedMeshVersion_ = meshes_->version;
}

void Renderer::createResources(const MeshLibrary&) {
    uploadMeshes();

    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_));
    // One set per texture
    VkDescriptorPoolSize texSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096};
    dpci.maxSets = 4096;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &texSize;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &texturePool_));

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_));

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = MAX_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, cmd_));

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES; ++i) {
        ubo_[i] = createBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descPool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &descSets_[i]));

        VkDescriptorBufferInfo bi{ubo_[i].buffer, 0, sizeof(FrameUBO)};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = descSets_[i];
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &bi;
        VkDescriptorImageInfo shadowInfo{shadowSampler_, shadowMap_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[1].dstSet = descSets_[i];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &shadowInfo;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);

        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]));
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]));
    }

    const uint8_t white[4] = {255, 255, 255, 255};
    whiteTexture_ = createTexture(white, 1, 1);
    uploadTextures();
}

void Renderer::initImGui() {
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance_;
    info.PhysicalDevice = gpu_;
    info.Device = device_;
    info.QueueFamily = queueFamily_;
    info.Queue = queue_;
    info.DescriptorPoolSize = 64;
    info.MinImageCount = 2;
    info.ImageCount = (uint32_t)swapImages_.size();
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;   // UI is drawn on the resolved swapchain image
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat_;
    info.CheckVkResultFn = checkVk;
    ImGui_ImplVulkan_Init(&info);
    imguiInitialized_ = true;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void Renderer::recordCommands(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, ImDrawData* ui) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));

    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags depthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer_.buffer, &vbOffset);
    vkCmdBindIndexBuffer(cb, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 0, nullptr);

    // ---- Pass 0: shadow map (depth from the sun)
    if (frameShadows_ && info.items) {
        // The previous frame's sampling must finish first; the old contents are discarded
        imageBarrier(cb, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                     depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = shadowMap_.view;
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {1.f, 0};
        VkRenderingInfo sri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
        sri.layerCount = 1;
        sri.pDepthAttachment = &sd;
        vkCmdBeginRendering(cb, &sri);
        VkViewport sv{0.f, 0.f, (float)kShadowSize, (float)kShadowSize, 0.f, 1.f};
        VkRect2D ss{{0, 0}, {kShadowSize, kShadowSize}};
        vkCmdSetViewport(cb, 0, 1, &sv);
        vkCmdSetScissor(cb, 0, 1, &ss);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        for (const DrawItem& o : *info.items) {
            PushConstants pc{o.model, {}, {}};
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            const Mesh& mesh = meshes_->get(o.mesh);
            vkCmdDrawIndexed(cb, mesh.indexCount, 1, mesh.firstIndex, mesh.vertexOffset, 0);
        }
        vkCmdEndRendering(cb);
        imageBarrier(cb, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    imageBarrier(cb, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, 0, colorStage,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (colorMsaa_.image)
        imageBarrier(cb, colorMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    // ---- Pass 1: 3D scene (MSAA, resolved into the swapchain image)
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.clearValue.color = {{kSkySrgb.x, kSkySrgb.y, kSkySrgb.z, 1.f}};
    if (colorMsaa_.image) {
        color.imageView = colorMsaa_.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = swapViews_[imageIndex];
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = swapViews_[imageIndex];
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.f, 0};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    vkCmdBeginRendering(cb, &ri);

    stats_ = {};
    // Clamp the editor's viewport rectangle to the framebuffer
    float vx = std::clamp(info.viewportX, 0.f, (float)extent_.width);
    float vy = std::clamp(info.viewportY, 0.f, (float)extent_.height);
    float vw = std::min(info.viewportW, (float)extent_.width - vx);
    float vh = std::min(info.viewportH, (float)extent_.height - vy);
    if (info.items && vw >= 1.f && vh >= 1.f) {
        VkViewport viewport{vx, vy, vw, vh, 0.f, 1.f};
        VkRect2D scissor{{(int32_t)vx, (int32_t)vy}, {(uint32_t)vw, (uint32_t)vh}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        const auto& items = *info.items;
        VkDescriptorSet boundTexture = VK_NULL_HANDLE;
        auto draw = [&](const DrawItem& o, Vec3 rgb) {
            bool textured = o.texture >= 0 && o.texture < (int)gpuTextures_.size();
            VkDescriptorSet set = textured ? gpuTextures_[(size_t)o.texture].set : whiteTexture_.set;
            if (set != boundTexture) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &set, 0, nullptr);
                boundTexture = set;
            }
            PushConstants pc{o.model, {rgb.x, rgb.y, rgb.z, 1.f}, {o.checker ? 1.f : 0.f, o.tiling, textured ? 1.f : 0.f, 0.f}};
            vkCmdPushConstants(cb, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            const Mesh& mesh = meshes_->get(o.mesh);
            vkCmdDrawIndexed(cb, mesh.indexCount, 1, mesh.firstIndex, mesh.vertexOffset, 0);
            stats_.drawCalls++;
            stats_.triangles += mesh.indexCount / 3;
        };

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline_);
        for (const DrawItem& o : items) draw(o, srgbToLinear(o.color));

        for (const DrawItem& o : items) {
            if (!o.outlined) continue;
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, stencilPipeline_);
            draw(o, kOutlineSrgb);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline_);
            draw(o, kOutlineSrgb);
        }
    }
    vkCmdEndRendering(cb);

    // ---- Pass 2: ImGui directly on the swapchain image
    imageBarrier(cb, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, colorStage,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (ui) {
        VkRenderingAttachmentInfo uiColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        uiColor.imageView = swapViews_[imageIndex];
        uiColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        uiColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        uiColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo uri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        uri.renderArea = {{0, 0}, extent_};
        uri.layerCount = 1;
        uri.colorAttachmentCount = 1;
        uri.pColorAttachments = &uiColor;
        vkCmdBeginRendering(cb, &uri);
        ImGui_ImplVulkan_RenderDrawData(ui, cb);
        vkCmdEndRendering(cb);
    }

    if (!screenshotPath_.empty() && canReadSwapchain_) {
        // Copy the finished image into a host-visible buffer (saved after the frame completes)
        VkDeviceSize size = (VkDeviceSize)extent_.width * extent_.height * 4;
        if (!readback_.buffer || readback_.size < size) {
            if (readback_.buffer) destroyBuffer(readback_);
            readback_ = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            readback_.size = size;
        }
        imageBarrier(cb, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cb, swapImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                               &region);
        imageBarrier(cb, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    } else {
        imageBarrier(cb, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    VK_CHECK(vkEndCommandBuffer(cb));
}

void Renderer::saveScreenshot() {
    std::string path = std::move(screenshotPath_);
    screenshotPath_.clear();
    if (!canReadSwapchain_) {
        std::fprintf(stderr, "screenshot: this swapchain can't be read back\n");
        screenshotResult_ = -1;
        return;
    }
    vkQueueWaitIdle(queue_);   // the copy has finished
    std::vector<uint8_t> rgba((size_t)extent_.width * extent_.height * 4);
    std::memcpy(rgba.data(), readback_.mapped, rgba.size());
    bool bgr = swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapFormat_ == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (bgr) std::swap(rgba[i], rgba[i + 2]);
        rgba[i + 3] = 255;
    }
    bool ok = writePng(path, extent_.width, extent_.height, rgba.data());
    std::printf(ok ? "Saved screenshot %s (%ux%u)\n" : "screenshot: cannot write %s\n", path.c_str(), extent_.width,
                extent_.height);
    screenshotResult_ = ok ? 1 : -1;
}

void Renderer::render(const SceneRenderInfo& info, ImDrawData* ui) {
    if (width_ == 0 || height_ == 0) return;
    if (resized_) recreateSwapchain();
    if (meshes_->version != uploadedMeshVersion_) uploadMeshes();
    if ((int)gpuTextures_.size() < textures_->count()) uploadTextures();

    VK_CHECK(vkWaitForFences(device_, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX));

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[frame_], VK_NULL_HANDLE,
                                       &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    VK_CHECK(vkResetFences(device_, 1, &inFlight_[frame_]));

    // Lights: the first directional light is the sun (a default one if there's none), then the point lights
    // nearest the camera
    LightInfo sun;
    sun.directional = true;
    sun.direction = normalize({-0.45f, -1.f, -0.3f});
    sun.color = {1.f, 0.95f, 0.85f};
    std::vector<const LightInfo*> points;
    bool haveSun = false;
    if (info.lights)
        for (const LightInfo& l : *info.lights) {
            if (l.directional && !haveSun) { sun = l; haveSun = true; }
            else if (!l.directional) points.push_back(&l);
        }
    std::sort(points.begin(), points.end(), [&](const LightInfo* a, const LightInfo* b) {
        return length(a->position - info.camPos) < length(b->position - info.camPos);
    });
    if (points.size() > (size_t)kMaxPointLights) points.resize(kMaxPointLights);
    frameShadows_ = sun.castShadows && sun.intensity > 0.f;

    Vec3 fog = srgbToLinear(kSkySrgb);
    FrameUBO u{};
    u.viewProj = info.viewProj;
    u.lightViewProj = shadowMatrix(info, normalize(sun.direction));
    set4(u.camPos, info.camPos, 1.f);
    set4(u.fogColor, fog, 0.006f);
    u.viewport[0] = info.viewportW;
    u.viewport[1] = info.viewportH;
    u.viewport[2] = 2.5f;
    set4(u.sunDir, normalize(sun.direction), frameShadows_ ? 1.f : 0.f);
    set4(u.sunColor, srgbToLinear(sun.color) * sun.intensity, 0.f);
    set4(u.ambient, {0.45f, 0.45f, 0.45f}, (float)points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        set4(u.pointPos[i], points[i]->position, std::max(points[i]->range, 0.01f));
        set4(u.pointColor[i], srgbToLinear(points[i]->color) * points[i]->intensity, 0.f);
    }
    std::memcpy(ubo_[frame_].mapped, &u, sizeof(u));

    VK_CHECK(vkResetCommandBuffer(cmd_[frame_], 0));
    recordCommands(cmd_[frame_], imageIndex, info, ui);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[frame_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_[frame_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone_[imageIndex];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, inFlight_[frame_]));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    if (!screenshotPath_.empty()) saveScreenshot();
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || resized_) recreateSwapchain();
    else VK_CHECK(r);

    frame_ = (frame_ + 1) % MAX_FRAMES;
}

void Renderer::shutdown() {
    vkDeviceWaitIdle(device_);
    if (imguiInitialized_) ImGui_ImplVulkan_Shutdown();
    destroySwapchainResources();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    for (VkPipeline p : {scenePipeline_, stencilPipeline_, outlinePipeline_, shadowPipeline_})
        vkDestroyPipeline(device_, p, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, textureSetLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descPool_, nullptr);
    for (GpuTexture& t : gpuTextures_) destroyTexture(t);
    destroyTexture(whiteTexture_);
    vkDestroyDescriptorPool(device_, texturePool_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroySampler(device_, shadowSampler_, nullptr);
    destroyImage(shadowMap_);
    destroyBuffer(vertexBuffer_);
    destroyBuffer(indexBuffer_);
    if (readback_.buffer) destroyBuffer(readback_);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        destroyBuffer(ubo_[i]);
        vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        vkDestroyFence(device_, inFlight_[i], nullptr);
    }
    vkDestroyCommandPool(device_, cmdPool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) {
        auto destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(instance_, messenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
}
