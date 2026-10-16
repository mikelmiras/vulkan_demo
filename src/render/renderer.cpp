#include "render/renderer.h"

#include <atomic>
#include <chrono>

#include "core/vfs.h"
#include "render/renderer_internal.h"

#include "core/hdr_image.h"
#include "core/png.h"

#include <cstddef>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "core/log.h"
#include "core/profiler.h"
#include "platform/platform.h"


[[noreturn]] static void fatal(const char* msg) {
    std::fprintf(stderr, "fatal: %s\n", msg);
    std::exit(1);
}

namespace {

using namespace render_detail;

std::atomic<int> g_validationErrors{0};   // reported by the validation layer (see Renderer::validationErrors)

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) logLine("[validation] %s", data->pMessage);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++g_validationErrors;
    return VK_FALSE;
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
    if (const char* v = std::getenv("VKWORLD_UI_LAYER"); v && !std::strcmp(v, "1")) uiLayerForced_ = true;
    if (const char* v = std::getenv("VKWORLD_PREPASS"); v && !std::strcmp(v, "0")) prepass_ = false;
    if (const char* v = std::getenv("VKWORLD_SHADOW_CACHE"); v && !std::strcmp(v, "0")) shadowCacheOn_ = false;
    if (const char* v = std::getenv("VKWORLD_RENDER_SCALE"))   // (before the post images: the upscaler's are made with them)
        renderScaleSet_ = scaleNow_ = std::clamp((float)std::atof(v), 0.5f, 1.f);
    if (const char* v = std::getenv("VKWORLD_QUALITY")) {
        if (!std::strcmp(v, "low")) quality_ = Quality::Low;
        else if (!std::strcmp(v, "medium")) quality_ = Quality::Medium;
    }
    if (const char* v = std::getenv("VKWORLD_HDR_PEAK"); v && std::atof(v) > 0) {
        hdrPeak_ = (float)std::atof(v);
        hdrPeakForced_ = true;
    }
    if (const char* v = std::getenv("VKWORLD_HDR_PAPER_WHITE"); v && std::atof(v) > 0) hdrPaperWhite_ = (float)std::atof(v);

    createInstance();
    surface_ = platform::createVulkanSurface(instance_);
    if (!surface_) fatal("cannot create Vulkan window surface");
    pickDevice();
    createDevice();
    createPipelineCache();
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
    validation = validation && validationAllowed_;

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
    {   // HDR colour spaces on the surface, where the loader has them
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
        for (const auto& p : props)
            if (!std::strcmp(p.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
                exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
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
    std::string preference = gpuPreference_;
    if (const char* env = std::getenv("VKWORLD_GPU"); env && *env) preference = env;
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

        // (the preference first: integrated for less power, a name; then discrete before integrated before others)
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                 : 1;
        if (preference == "integrated" && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10;
        else if (!preference.empty() && preference != "integrated" && preference != "discrete") {
            std::string name = props.deviceName, want = preference;
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            for (char& c : want) c = (char)std::tolower((unsigned char)c);
            if (name.find(want) != std::string::npos) score += 10;
        }
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
    logfile::write(std::string("GPU: ") + props.deviceName);
    {   // GPU pass timings: timestamps on the graphics queue
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qp(n);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &n, qp.data());
        timestampBits_ = queueFamily_ < n ? qp[queueFamily_].timestampValidBits : 0;
        timestampPeriod_ = props.limits.timestampPeriod;
    }
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
    VkFormatProperties dfp;
    vkGetPhysicalDeviceFormatProperties(gpu_, depthFormat_, &dfp);
    aoSupported_ = (dfp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    maxSamples_ = samples_;
    // VKWORLD_AA=taa / msaa / off: the anti-aliasing to start with (programs can change it later)
    if (const char* aa = std::getenv("VKWORLD_AA")) {
        std::string want = aa;
        if (want == "taa" && aoSupported_) aa_ = Antialiasing::Temporal;
        else if (want == "off") aa_ = Antialiasing::Off;
    }
    if (aa_ != Antialiasing::Msaa) samples_ = VK_SAMPLE_COUNT_1_BIT;
}

// The pipeline cache: the last run's data when it was made by this GPU and driver (its header says), else empty
void Renderer::createPipelineCache() {
    std::string data;
    if (!pipelineCachePath_.empty()) {
        std::ifstream f(pipelineCachePath_, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    struct Header {
        uint32_t size, version, vendor, device;
        uint8_t uuid[VK_UUID_SIZE];
    } h{};
    bool usable = data.size() >= sizeof(Header);
    if (usable) {
        std::memcpy(&h, data.data(), sizeof(Header));
        usable = h.version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h.vendor == props.vendorID && h.device == props.deviceID &&
                 std::memcmp(h.uuid, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (usable) {
        ci.initialDataSize = data.size();
        ci.pInitialData = data.data();
    }
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipelineCache_) != VK_SUCCESS) pipelineCache_ = VK_NULL_HANDLE;
}

void Renderer::savePipelineCache() {
    if (!pipelineCache_ || pipelineCachePath_.empty()) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::string data(size, '\0');
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(pipelineCachePath_.parent_path(), ec);
    std::filesystem::path tmp = pipelineCachePath_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(data.data(), (std::streamsize)size);
        if (!f) return;
    }
    std::filesystem::rename(tmp, pipelineCachePath_, ec);
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
    if (supported.fragmentStoresAndAtomics) f2.features.fragmentStoresAndAtomics = VK_TRUE;   // (texture streaming)
    if (supported.textureCompressionBC) f2.features.textureCompressionBC = VK_TRUE;           // (cooked textures)
    const char* bc = std::getenv("VKWORLD_BC");   // (=0: as if the GPU had none: cooked textures decoded at load)
    bcSupported_ = supported.textureCompressionBC && !(bc && std::strcmp(bc, "0") == 0);
    streamingSupported_ = supported.fragmentStoresAndAtomics;

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Portability drivers (MoltenVK etc.) require VK_KHR_portability_subset to be enabled when they expose it
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &extCount, available.data());
    for (const auto& e : available) {
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) exts.push_back("VK_KHR_portability_subset");
        if (std::strcmp(e.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0) {   // (telling HDR displays)
            exts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            hdrMetadataSupported_ = true;
        }
    }

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

Renderer::Buffer Renderer::createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VK_CHECK(vkCreateBuffer(device_, &ci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));
    b.size = size;
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
                                      uint32_t mipLevels, VkImageCreateFlags flags) {
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
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

    {   // UNORM target: shaders output gamma-encoded colour, which keeps ImGui's sRGB colours correct; or HDR
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &count, nullptr);
        surfaceFormats_.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &count, surfaceFormats_.data());
        if (sdrFormat_ == VK_FORMAT_UNDEFINED) sdrFormat_ = display::choose(surfaceFormats_, false).format.format;
        display::Choice c = display::choose(surfaceFormats_, hdrWanted_ && screenshotPath_.empty());
        swapFormat_ = c.format.format;
        swapColorSpace_ = c.format.colorSpace;
        output_ = c.output;
        uiLayered_ = output_ != display::Output::Sdr || uiLayerForced_;
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
    ci.imageColorSpace = swapColorSpace_;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    canReadSwapchain_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && output_ == display::Output::Sdr;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (canReadSwapchain_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);
    if (output_ != display::Output::Sdr) describeHdr();

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapImages_.data());
    for (VkImage img : swapImages_) swapViews_.push_back(createView(img, swapFormat_, VK_IMAGE_ASPECT_COLOR_BIT));

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderDone_.resize(count);
    for (auto& s : renderDone_) VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &s));

    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        colorMsaa_ = createImage(hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
    depth_ = createImage(depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (aoSupported_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (aoSupported_) depthSampleView_ = createView(depth_.image, depthFormat_, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::destroySwapchainResources() {
    for (VkImageView v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
    swapViews_.clear();
    renderDone_.clear();
    if (colorMsaa_.image) destroyImage(colorMsaa_);
    if (depthSampleView_) vkDestroyImageView(device_, depthSampleView_, nullptr);
    depthSampleView_ = VK_NULL_HANDLE;
    destroyImage(depth_);
}

void Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(device_);
    ssrValid_ = false;   // (the picture is remade)
    destroySwapchainResources();
    destroyPostImages();
    VkFormat before = swapFormat_;
    createSwapchain();
    if (swapFormat_ != before && compositePipeline_) {   // (HDR on or off: the composite writes another format)
        vkDestroyPipeline(device_, compositePipeline_, nullptr);
        compositePipeline_ = createPostPipeline("composite.frag.spv", swapFormat_, false);
    }
    createPostImages();
    resized_ = false;
}

// HDR output: the display's real peak (unless VKWORLD_HDR_PEAK says), and the metadata telling it about the picture
void Renderer::describeHdr() {
    display::Luminance screen;
    if (platform::displayLuminance(screen.peak, screen.frameAverage, screen.min) && !hdrPeakForced_)
        hdrPeak_ = std::clamp(screen.peak, 100.f, 10000.f);
    displayLuminance_ = screen;
    if (!hdrMetadataSupported_) return;
    auto set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(device_, "vkSetHdrMetadataEXT");
    if (!set) return;
    display::Metadata m = display::metadataFor(output_, hdrPeak_, screen);
    VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    md.displayPrimaryRed = {m.red[0], m.red[1]};
    md.displayPrimaryGreen = {m.green[0], m.green[1]};
    md.displayPrimaryBlue = {m.blue[0], m.blue[1]};
    md.whitePoint = {m.white[0], m.white[1]};
    md.maxLuminance = m.maxLuminance;
    md.minLuminance = m.minLuminance;
    md.maxContentLightLevel = m.maxContentLight;
    md.maxFrameAverageLightLevel = m.maxFrameAverageLight;
    set(device_, 1, &swapchain_, &md);
}

void Renderer::setHdrOutput(bool on) {
    if (on == hdrWanted_) return;
    hdrWanted_ = on;
    resized_ = true;   // (the swapchain is made again after this frame)
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
    for (const fs::path& dir : {exeDir / "shaders", exeDir.parent_path() / "shaders"})   // (or in a packed game)
        if (vfs::exists(dir / name)) { path = dir / name; break; }
    std::string bytes;
    if (path.empty() || !vfs::read(path, bytes)) {
        std::fprintf(stderr, "cannot find shader %s next to %s\n", name.c_str(), exeDir.u8string().c_str());
        std::exit(1);
    }
    size_t size = bytes.size();
    std::vector<uint32_t> code((size + 3) / 4);
    std::memcpy(code.data(), bytes.data(), size);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &mod));
    return mod;
}

VkPipeline Renderer::createPipeline(const PipelineDesc& d) {
    const bool depthOnly = d.shadow || d.fragShader == nullptr;   // the shadow pass
    const bool colorless = depthOnly || d.noColor;
    VkShaderModule vs = loadShader(d.vertShader), fs = d.fragShader ? loadShader(d.fragShader) : VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };

    // Binding 0: the vertices; binding 1: their skinning data (every vertex shader skins)
    VkVertexInputBindingDescription vib[2] = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                              {1, sizeof(SkinVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    const VkVertexInputAttributeDescription all[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
    };
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (uint32_t i = 0; i < 3; ++i)
        if (d.attributeMask ? (d.attributeMask >> i) & 1u : i < d.vertexAttributes) attrs.push_back(all[i]);
    attrs.push_back({3, 1, VK_FORMAT_R16G16B16A16_UINT, offsetof(SkinVertex, joints)});
    attrs.push_back({4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SkinVertex, weights)});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vib;
    vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();

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
    if (d.blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    const uint32_t colours = colorless ? 0 : d.colorFormat2 != VK_FORMAT_UNDEFINED ? 2 : 1;
    VkPipelineColorBlendAttachmentState cbas[2] = {cba, cba};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = colours;
    cb.pAttachments = cbas;

    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat formats[2] = {d.colorFormat != VK_FORMAT_UNDEFINED ? d.colorFormat : hdrFormat_, d.colorFormat2};
    rci.colorAttachmentCount = colours;
    rci.pColorAttachmentFormats = formats;   // (the scene: the HDR image)
    rci.depthAttachmentFormat = depthOnly ? shadowFormat_ : depthFormat_;
    rci.stencilAttachmentFormat = depthOnly ? VK_FORMAT_UNDEFINED : depthFormat_;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = fs ? 2 : 1;
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
    VK_CHECK(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &pipeline));

    vkDestroyShaderModule(device_, vs, nullptr);
    if (fs) vkDestroyShaderModule(device_, fs, nullptr);
    return pipeline;
}

void Renderer::createPipelines() {
    // Set 0, per frame: the frame data, the shadow map and the instances
    const VkShaderStageFlags fragFog = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;   // (the fog's passes)
    VkDescriptorSetLayoutBinding frameBindings[37] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | fragFog, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // joint matrices (and terrains' layers)
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},   // particles
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragFog, nullptr},   // the shadow atlas (the fog's beams too)
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // spot lights' cookies
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // the sky image
        {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // its radiance
        {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // reflection probes
        {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},   // instance indices (GPU culling)
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fragFog, nullptr},   // point and spot lights
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, fragFog, nullptr},   // their clusters
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},     // foliage instances
        {17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // GI probes (renderer_gi.cpp)
        {18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // last frame (reflections)
        {19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // texture streaming's reports
        {20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragFog, nullptr},   // the sun's cascades' depths
        {21, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // volumetric fog
        {22, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // ... its columns' surfaces
        {23, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // the physical sky's table
        {24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // decals
        {25, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDecalTextures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // ... their textures
        {26, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // water's refraction
        {27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // decals' clusters
        {28, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDecalNormals, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // ... normal maps
        // The half-size traces (renderer_trace.cpp): the depth, the surface, this frame's results, last frame's, the
        // traced pixels' distances, the motion
        {29, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {30, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {31, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {33, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {34, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {35, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {36, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 37;
    dslci.pBindings = frameBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_));
    // Set 1, per draw: the material's maps (albedo, normal, metal/rough, emissive, height, detail normal)
    VkDescriptorSetLayoutBinding matBindings[6];
    for (uint32_t i = 0; i < 6; ++i)
        matBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    dslci.bindingCount = 6;
    dslci.pBindings = matBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &materialSetLayout_));

    VkDescriptorSetLayout layouts[2] = {setLayout_, materialSetLayout_};
    // The shadow pass: which shadow camera (the first 16 bytes); foliage: its terrain, colour and wind (all 112)
    VkPushConstantRange shadowLayer{VK_SHADER_STAGE_VERTEX_BIT, 0, 112};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = layouts;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &shadowLayer;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_));

    // Depth from the sun's point of view (no fragment shader)
    shadowPipeline_ = createPipeline({"shadow.vert.spv", nullptr, true, true, false, VK_CULL_MODE_NONE, false,
                                      VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0, 3, false, true, 0b101});   // (no normals)
    // ... and for casters with holes: a fragment shader drops what the texture's alpha says is open
    shadowCutoutPipeline_ = createPipeline({"shadow.vert.spv", "shadow_cutout.frag.spv", true, true, false, VK_CULL_MODE_NONE,
                                            false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0, 3, false, true, 0b101});

    // Post-processing: one or two images to read, parameters in push constants
    VkDescriptorSetLayoutBinding postBindings[6] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // (composite: AO)
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // (composite: the UI layer)
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // (composite: auto-exposure)
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},   // (composite: luminance)
    };
    dslci.bindingCount = 6;
    dslci.pBindings = postBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &postSetLayout_));
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &postSetLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &postLayout_));
    // The temporal anti-aliasing (and upscaling) pass: the same images, more push constants (a matrix, the scale)
    VkPushConstantRange taaRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 112};
    pl.pPushConstantRanges = &taaRange;
    VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &taaLayout_));
    taaPipeline_ = createPostPipeline("taa.frag.spv", hdrFormat_, false, taaLayout_);
    taaUpscalePipeline_ = createPostPipeline("taa_upscale.frag.spv", hdrFormat_, false, taaLayout_);
    bloomDownPipeline_ = createPostPipeline("bloom_down.frag.spv", hdrFormat_, false);
    lumPipeline_ = createPostPipeline("luminance.frag.spv", VK_FORMAT_R16_SFLOAT, false);
    // (the half-size traces: the scene's frame data, set 0; renderer_trace.cpp)
    ssrTracePipeline_ = createPostPipeline("ssr_trace.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false, pipelineLayout_,
                                           VK_FORMAT_R32G32_SFLOAT);
    giTracePipeline_ = createPostPipeline("gi_trace.frag.spv", VK_FORMAT_R16G16B16A16_SFLOAT, false, pipelineLayout_);
    adaptPipeline_ = createPostPipeline("adapt.frag.spv", VK_FORMAT_R32_SFLOAT, false);
    bloomUpPipeline_ = createPostPipeline("bloom_up.frag.spv", hdrFormat_, true);
    compositePipeline_ = createPostPipeline("composite.frag.spv", swapFormat_, false);
    compositeViewPipeline_ = createPostPipeline("composite.frag.spv", sdrFormat_, false);
    createSampledPipelines();
}

// The pipelines drawing into the scene's (maybe multisampled) targets, and the AO pass reading its depth
void Renderer::createSampledPipelines() {
    // Lit scene objects: depth tested, no stencil
    scenePipeline_ = createPipeline({"scene.vert.spv", "scene.frag.spv", true, true, true, VK_CULL_MODE_BACK_BIT,
                                     false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0});
    // See-through objects: depth-tested, but not hiding what's drawn after them
    transparentPipeline_ = createPipeline({"scene.vert.spv", "scene.frag.spv", true, false, true, VK_CULL_MODE_BACK_BIT,
                                           false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0, 3, true});
    // Foliage: instanced plants (two-sided), lit like everything else
    foliagePipeline_ = createPipeline({"foliage.vert.spv", "scene.frag.spv", true, true, true, VK_CULL_MODE_NONE,
                                       false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0});
    // The depth prepass and the lit pass after it (depth tested before shading, not written again)
    PipelineDesc depth{"scene.vert.spv", "depth_prepass.frag.spv", true, true, false, VK_CULL_MODE_BACK_BIT,
                       false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0};
    depth.noColor = true;
    prepassPipeline_ = createPipeline(depth);
    depth.vertShader = "foliage.vert.spv";
    depth.cull = VK_CULL_MODE_NONE;
    prepassFoliagePipeline_ = createPipeline(depth);
    if (samples_ == VK_SAMPLE_COUNT_1_BIT) {   // (with temporal AA: the motion vectors too)
        depth.fragShader = "depth_prepass_velocity.frag.spv";
        depth.noColor = false;
        depth.colorWrite = true;
        depth.colorFormat = VK_FORMAT_R16G16_SFLOAT;
        depth.colorFormat2 = VK_FORMAT_R16G16B16A16_SFLOAT;   // (and the surface, for the half-size traces)
        prepassFoliageVelocityPipeline_ = createPipeline(depth);
        depth.vertShader = "scene.vert.spv";
        depth.cull = VK_CULL_MODE_BACK_BIT;
        prepassVelocityPipeline_ = createPipeline(depth);
    }
    sceneEarlyPipeline_ = createPipeline({"scene.vert.spv", "scene_early.frag.spv", true, false, true, VK_CULL_MODE_BACK_BIT,
                                          false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0});
    foliageEarlyPipeline_ = createPipeline({"foliage.vert.spv", "scene_early.frag.spv", true, false, true, VK_CULL_MODE_NONE,
                                            false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0});
    // Selected object's full silhouette into stencil (ignores depth so occluded parts are marked too)
    stencilPipeline_ = createPipeline({"scene.vert.spv", "outline.frag.spv", false, false, false, VK_CULL_MODE_NONE,
                                       true, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, 0xff});
    // Hull extruded in screen space, drawn only outside the silhouette -> outline visible through walls
    outlinePipeline_ = createPipeline({"outline.vert.spv", "outline.frag.spv", false, false, true, VK_CULL_MODE_NONE,
                                       true, VK_COMPARE_OP_NOT_EQUAL, VK_STENCIL_OP_KEEP, 0, 2});
    if (aoSupported_) {
        aoPipeline_ = createPostPipeline(samples_ != VK_SAMPLE_COUNT_1_BIT ? "ssao_ms.frag.spv" : "ssao.frag.spv",
                                         VK_FORMAT_R8_UNORM, false);
        ssrCapturePipeline_ = createPostPipeline(samples_ != VK_SAMPLE_COUNT_1_BIT ? "ssr_capture_ms.frag.spv" : "ssr_capture.frag.spv",
                                                 hdrFormat_, false);
    }
    particleAlphaPipeline_ = createParticlePipeline(false);
    particleAdditivePipeline_ = createParticlePipeline(true);
    skyPipeline_ = createSkyPipeline();
}

void Renderer::destroySampledPipelines() {
    for (VkPipeline* p : {&scenePipeline_, &transparentPipeline_, &stencilPipeline_, &outlinePipeline_, &aoPipeline_, &ssrCapturePipeline_,
                          &particleAlphaPipeline_, &particleAdditivePipeline_, &skyPipeline_, &foliagePipeline_,
                          &prepassPipeline_, &prepassFoliagePipeline_, &sceneEarlyPipeline_, &foliageEarlyPipeline_,
                          &prepassVelocityPipeline_, &prepassFoliageVelocityPipeline_}) {
        if (*p) vkDestroyPipeline(device_, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
}

VkPipeline Renderer::createSkyPipeline() {
    VkShaderModule vs = loadShader("sky.vert.spv"), fs = loadShader("sky.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;    // at the far plane: only where nothing was drawn
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &hdrFormat_;
    rci.depthAttachmentFormat = depthFormat_;
    rci.stencilAttachmentFormat = depthFormat_;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = 2;
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
    VK_CHECK(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &pipeline));
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return pipeline;
}

VkPipeline Renderer::createParticlePipeline(bool additive) {
    VkShaderModule vs = loadShader("particle.vert.spv"), fs = loadShader("particle.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;    // hidden behind things
    ds.depthWriteEnable = VK_FALSE;  // but they don't hide each other
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Premultiplied colour; the alpha channel (the outline marker) is left alone
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &hdrFormat_;
    rci.depthAttachmentFormat = depthFormat_;
    rci.stencilAttachmentFormat = depthFormat_;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = 2;
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
    VK_CHECK(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &ci, nullptr, &pipeline));
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    return pipeline;
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
    cascades_ = createShadowArray(kCascadeSize, kCascades);
    atlas_ = createShadowArray(kAtlasSize, 1);
    createCookies();
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;   // the shader compares and filters itself
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &shadowSampler_));
    // ... and comparing in hardware, with bilinear filtering of the results (each tap a 2 x 2 PCF)
    VkFormatProperties shadowProps;
    vkGetPhysicalDeviceFormatProperties(gpu_, shadowFormat_, &shadowProps);
    bool linear = (shadowProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    sci.magFilter = sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;   // (where the format allows it)
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &shadowCompareSampler_));
    sci.compareEnable = VK_FALSE;

    // Textures: trilinear, repeating, anisotropic where available
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.anisotropyEnable = maxAnisotropy_ > 1.f;
    sci.maxAnisotropy = maxAnisotropy_;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler_));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;   // ... and blocky (pixel art), nearest mip
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &nearestSampler_));

    // Post-processing: bilinear, clamped, one level at a time
    VkSamplerCreateInfo post{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    post.magFilter = post.minFilter = VK_FILTER_LINEAR;
    post.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    post.addressModeU = post.addressModeV = post.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device_, &post, nullptr, &postSampler_));
    post.maxLod = VK_LOD_CLAMP_NONE;   // (auto-exposure's luminance: read at any mip)
    VK_CHECK(vkCreateSampler(device_, &post, nullptr, &lumSampler_));
}

// Meshes are only ever appended to the library, so only the new vertices and indices are copied, into room the
// buffers keep spare (they're host-visible: no transfer, and frames in flight only read the older ranges). Only
// growing past that room makes new buffers, which waits for the GPU.
void Renderer::uploadMeshes() {
    const size_t nv = meshes_->vertices.size(), ni = meshes_->indices.size();
    if (nv < uploadedVertices_ || ni < uploadedIndices_) {   // a different library: everything again
        vkDeviceWaitIdle(device_);
        uploadedVertices_ = uploadedIndices_ = 0;
    }
    if (!vertexBuffer_.buffer || nv > vertexCapacity_ || ni > indexCapacity_) {
        if (vertexBuffer_.buffer) {   // replacing: the old buffers may still be in use by frames in flight
            vkDeviceWaitIdle(device_);
            destroyBuffer(vertexBuffer_);
            destroyBuffer(skinBuffer_);
            destroyBuffer(indexBuffer_);
        }
        vertexCapacity_ = std::max<size_t>(nv + nv / 2, 4096);
        indexCapacity_ = std::max<size_t>(ni + ni / 2, 4096);
        vertexBuffer_ = createBuffer(vertexCapacity_ * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, nullptr);
        skinBuffer_ = createBuffer(vertexCapacity_ * sizeof(SkinVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, nullptr);
        indexBuffer_ = createBuffer(indexCapacity_ * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, nullptr);
        uploadedVertices_ = uploadedIndices_ = 0;
    }
    auto* verts = static_cast<Vertex*>(vertexBuffer_.mapped);
    auto* skin = static_cast<SkinVertex*>(skinBuffer_.mapped);
    std::memcpy(verts + uploadedVertices_, meshes_->vertices.data() + uploadedVertices_,
                (nv - uploadedVertices_) * sizeof(Vertex));
    for (size_t i = uploadedVertices_; i < nv; ++i)   // (kept aligned with the vertices; zero in case it isn't)
        skin[i] = i < meshes_->skin.size() ? meshes_->skin[i] : SkinVertex{};
    std::memcpy(static_cast<uint32_t*>(indexBuffer_.mapped) + uploadedIndices_, meshes_->indices.data() + uploadedIndices_,
                (ni - uploadedIndices_) * sizeof(uint32_t));
    // Vertices rewritten in place since (a terrain being sculpted: frames in flight may see half of a stroke's step)
    for (const auto& [first, entry] : meshes_->rewritten) {
        if (entry.second <= uploadedRewriteSerial_ || first >= uploadedVertices_) continue;   // (new ones: copied above)
        size_t count = std::min<size_t>(entry.first, uploadedVertices_ - first);
        std::memcpy(verts + first, meshes_->vertices.data() + first, count * sizeof(Vertex));
        for (size_t i = first; i < first + count; ++i) skin[i] = i < meshes_->skin.size() ? meshes_->skin[i] : SkinVertex{};
    }
    uploadedRewriteSerial_ = meshes_->rewriteSerial;
    uploadedVertices_ = nv;
    uploadedIndices_ = ni;
    uploadedMeshVersion_ = meshes_->version;
}

void Renderer::createResources(const MeshLibrary&) {
    uploadMeshes();

    VkDescriptorPoolSize poolSizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, MAX_FRAMES},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (24 + kDecalTextures + kDecalNormals) * MAX_FRAMES},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 * MAX_FRAMES}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = MAX_FRAMES;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_));
    // Material sets (four maps each), made as materials are first drawn
    VkDescriptorPoolSize matSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * 4096};
    dpci.maxSets = 4096;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &matSize;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &materialPool_));

    // Post-processing sets: prefilter, the downsamples, the upsamples and the composite
    // (+2 per view: the temporal pass's two directions; +2: auto-exposure's luminance and adaptation; +1: the sky's)
    const uint32_t postSetCount = 2 * kMaxBloomMips + 9 + 2 * kMaxViews;
    VkDescriptorPoolSize postSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * postSetCount};
    dpci.maxSets = postSetCount;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &postSize;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &postPool_));
    std::vector<VkDescriptorSetLayout> postLayouts(postSetCount, postSetLayout_);
    std::vector<VkDescriptorSet> postSets(postLayouts.size());
    VkDescriptorSetAllocateInfo pai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    pai.descriptorPool = postPool_;
    pai.descriptorSetCount = (uint32_t)postLayouts.size();
    pai.pSetLayouts = postLayouts.data();
    VK_CHECK(vkAllocateDescriptorSets(device_, &pai, postSets.data()));
    prefilterSet_ = postSets[0];
    compositeSet_ = postSets[1];
    aoSet_ = postSets[2 + 2 * kMaxBloomMips];
    taa_.sets[0] = postSets[3 + 2 * kMaxBloomMips];
    taa_.sets[1] = postSets[4 + 2 * kMaxBloomMips];
    for (int v = 0; v < kMaxViews; ++v)   // (the extra views' temporal passes)
        for (int k = 0; k < 2; ++k) viewImages_[v].taa.sets[k] = postSets[5 + 2 * kMaxBloomMips + 2 * v + k];
    ssrSet_ = postSets[5 + 2 * kMaxBloomMips + 2 * kMaxViews];
    lumSet_ = postSets[6 + 2 * kMaxBloomMips + 2 * kMaxViews];
    adaptSet_ = postSets[7 + 2 * kMaxBloomMips + 2 * kMaxViews];
    skyViewSet_ = postSets[8 + 2 * kMaxBloomMips + 2 * kMaxViews];
    {   // (the luminance image is mipped by blits: its format must allow filtered ones)
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu_, VK_FORMAT_R16_SFLOAT, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        lumSupported_ = (fp.optimalTilingFeatures & need) == need;
    }
    for (uint32_t m = 0; m < kMaxBloomMips; ++m) {
        downSets_[m] = postSets[2 + m];
        upSets_[m] = postSets[2 + kMaxBloomMips + m];
    }
    createPostImages();

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

    createFog();   // (its volumes: in the frame sets below)
    createSky();   // (its table too)
    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (!uboStride_) {   // (slots: this view's, then the extra views'; each at an offset the device allows)
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(gpu_, &props);
            VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
            uboStride_ = (sizeof(FrameUBO) + align - 1) / align * align;
        }
        ubo_[i] = createBuffer(uboStride_ * kUboSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descPool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &descSets_[i]));

        VkDescriptorBufferInfo bi{ubo_[i].buffer, 0, sizeof(FrameUBO)};
        VkWriteDescriptorSet w[4] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = descSets_[i];
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[0].pBufferInfo = &bi;
        VkDescriptorImageInfo cascadeInfo{shadowCompareSampler_, cascades_.array, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo spotInfo{shadowCompareSampler_, atlas_.layers[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo cascadeDepthInfo{shadowSampler_, cascades_.array, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[1].dstSet = descSets_[i];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &cascadeInfo;
        w[2] = w[1];
        w[2].dstBinding = 5;
        w[2].pImageInfo = &spotInfo;
        VkDescriptorImageInfo pointInfo{sampler_, cookies_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[3] = w[1];
        w[3].dstBinding = 6;
        w[3].pImageInfo = &pointInfo;
        VkWriteDescriptorSet more[4] = {w[1], w[1], w[1], w[1]};
        more[0].dstBinding = 20;
        more[0].pImageInfo = &cascadeDepthInfo;
        VkDescriptorImageInfo fogInfo{fogSampler_, fog_[i].volume.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        more[1].dstBinding = 21;
        more[1].pImageInfo = &fogInfo;
        VkDescriptorImageInfo surfaceInfo{fogSampler_, fogSurface_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        more[2].dstBinding = 22;
        more[2].pImageInfo = &surfaceInfo;
        vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
        VkDescriptorImageInfo skyInfo{postSampler_, skyView_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        more[3].dstBinding = 23;
        more[3].pImageInfo = &skyInfo;
        vkUpdateDescriptorSets(device_, 4, more, 0, nullptr);

        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]));
        VK_CHECK(vkCreateFence(device_, &fci, nullptr, &inFlight_[i]));
        if (timestampBits_) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = kMaxTimestamps;
            VK_CHECK(vkCreateQueryPool(device_, &qci, nullptr, &timestamps_[i]));
        }
    }

    createCulling();
    createLightBuffers();
    createDecalBuffers();
    createGi();
    writeSsrBinding();
    writeRefractionBinding();
    writeTraceBindings();
    // Texture streaming: on unless VKWORLD_TEXTURE_STREAMING=0; the budget from VKWORLD_TEXTURE_BUDGET (MB)
    if (const char* v = std::getenv("VKWORLD_TEXTURE_STREAMING"); v && std::strcmp(v, "0") == 0) streaming_ = false;
    if (!streamingSupported_) streaming_ = false;
    if (const char* v = std::getenv("VKWORLD_TEXTURE_BUDGET"); v && std::atoi(v) > 0)
        streamSettings_.budget = (uint64_t)std::atoi(v) << 20;
    ensureInstanceStore(1024);
    for (frame_ = 0; frame_ < MAX_FRAMES; ++frame_) {
        ensureIndexCapacity(1024);
        ensureFeedback(256);
        SceneRenderInfo none;
        uploadJoints(none);      // small buffers, so bindings 3 and 4 are always valid
        uploadParticles(none);
    }
    frame_ = 0;

    createEnvironmentResources();
    const uint8_t white[4] = {255, 255, 255, 255};
    whiteTexture_ = createTexture(white, 1, 1);
    const uint8_t flat[4] = {128, 128, 255, 255};
    flatNormal_ = createTexture(flat, 1, 1);
    uploadTextures();
}

int Renderer::validationErrors() { return g_validationErrors.load(); }

// A timestamp once everything recorded so far has finished on the GPU; the time since the previous one is `name`'s
void Renderer::timestamp(VkCommandBuffer cb, const char* name) {
    if (!timestamps_[frame_] || stampNames_[frame_].size() >= kMaxTimestamps) return;
    vkCmdWriteTimestamp(cb, stampNames_[frame_].empty() ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        timestamps_[frame_], (uint32_t)stampNames_[frame_].size());
    stampNames_[frame_].push_back(name);
}

// This frame slot's last timestamps (its fence has passed: they're done), as pass times for the profiler
void Renderer::readGpuTimes() {
    const std::vector<const char*>& names = stampNames_[frame_];
    if (!timestamps_[frame_] || names.size() < 2) return;
    uint64_t t[kMaxTimestamps] = {};
    if (vkGetQueryPoolResults(device_, timestamps_[frame_], 0, (uint32_t)names.size(), sizeof(t), t, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    uint64_t mask = timestampBits_ >= 64 ? ~0ull : (1ull << timestampBits_) - 1;
    std::vector<std::pair<std::string, float>> times;
    for (size_t i = 1; i < names.size(); ++i)
        times.emplace_back(names[i], (float)((double)((t[i] - t[i - 1]) & mask) * timestampPeriod_ / 1e6));
    gpuFrameMs_ = (float)((double)((t[names.size() - 1] - t[0]) & mask) * timestampPeriod_ / 1e6);
    profiler::setGpuTimes(std::move(times));
}

void Renderer::initImGui() {
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance_;
    info.PhysicalDevice = gpu_;
    info.Device = device_;
    info.PipelineCache = pipelineCache_;
    info.QueueFamily = queueFamily_;
    info.Queue = queue_;
    info.DescriptorPoolSize = 1024;   // (the font atlas, plus one per texture shown in the UI: images, thumbnails)
    info.MinImageCount = 2;
    info.ImageCount = (uint32_t)swapImages_.size();
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;   // UI is drawn on the resolved swapchain image
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &sdrFormat_;   // (the UI layer's too)
    info.CheckVkResultFn = checkVk;
    ImGui_ImplVulkan_Init(&info);
    imguiInitialized_ = true;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void Renderer::recordCommands(VkCommandBuffer cb, uint32_t imageIndex, const SceneRenderInfo& info, const SceneRenderInfo& scene,
                              ImDrawData* ui) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    uploadTextures(cb);   // new ones, a few megabytes a frame (until then they're drawn plain)
    if (uiLayerFresh_ && uiLayer_.image) {   // (the UI layer, just made: readable before it's drawn this frame)
        imageBarrier(cb, uiLayer_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    uiLayerFresh_ = false;
    recordSky(cb);   // (the physical sky's tables: before anything this frame reads them)
    if (lumFresh_) {   // (auto-exposure's images, just made: readable before anything samples them)
        for (VkImage image : {lum_.image, exposure_[0].image, exposure_[1].image})
            mipBarrier(cb, image, 0, image == lum_.image ? kLumMips : 1, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        lumFresh_ = false;
    }
    if (velocityFresh_ && velocity_.image) {   // (the prepass's outputs and the traces', just made: readable from now)
        for (VkImage image : {velocity_.image, gbuffer_.image, traced_[0].image, traced_[1].image, tracedLight_[0].image,
                              tracedLight_[1].image, tracedDepth_.image})
            mipBarrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        velocityFresh_ = false;
    }
    if (refractionFresh_ && refraction_.image) {   // (water's refraction picture, just made: readable from now)
        mipBarrier(cb, refraction_.image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        refractionFresh_ = false;
    }
    if (prevColorFresh_) {   // (reflections' picture, just made: readable, even before there's one)
        mipBarrier(cb, prevColor_.image, 0, prevMips_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        prevColorFresh_ = false;
    }
    for (ViewImage& v : viewImages_) {   // extra views' images made since the last frame: readable (black) from now
        if (!v.fresh) continue;
        imageBarrier(cb, v.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, v.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        imageBarrier(cb, v.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        v.fresh = false;
    }
    stampNames_[frame_].clear();
    if (timestamps_[frame_]) vkCmdResetQueryPool(cb, timestamps_[frame_], 0, kMaxTimestamps);
    timestamp(cb, "start");

    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags depthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkImageAspectFlags dsAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    VkBuffer vbs[2] = {vertexBuffer_.buffer, skinBuffer_.buffer};
    VkDeviceSize vbOffsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cb, 0, 2, vbs, vbOffsets);
    vkCmdBindIndexBuffer(cb, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    const uint32_t mainSlot = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &mainSlot);

    recordInstanceUpload(cb);   // (what changed in the instances: before anything reads them)
    recordCulling(cb, scene);   // (compute: before any rendering)

    // ---- Pass 0: shadow maps (the sun's cascades, spot lights)
    if (info.items) recordShadows(cb);
    timestamp(cb, "Shadows");

    // ---- Global illumination: the probes due this frame, drawn and projected
    recordGi(cb, info);

    // ---- The extra views, each whole into its own image (they use the same targets, before this view does)
    if (info.items && info.views)
        for (size_t v = 0; v < info.views->size() && v < (size_t)kMaxViews; ++v) recordView(cb, (int)v, (*info.views)[v]);
    if (info.views && !info.views->empty()) {
        const uint32_t mainSlot0 = 0;   // (the views bound their own slots)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &mainSlot0);
        timestamp(cb, "Other views");
    }

    // The previous frame's post passes may still be reading the HDR image: wait for them, discard its contents
    imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                 colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (colorMsaa_.image)
        imageBarrier(cb, colorMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 depthStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,   // (last frame's AO, pyramid)
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    // ---- The depth prepass (opaque things' depth: the lit pass then shades each pixel once), then the volumetric
    // fog, which looks at that depth (each cell column's surface: scene.frag weighs columns by it at edges)
    const float sx = std::clamp(scene.viewportX, 0.f, (float)extent_.width), sy = std::clamp(scene.viewportY, 0.f, (float)extent_.height);
    const float sw = std::min(scene.viewportW, (float)extent_.width - sx), sh = std::min(scene.viewportH, (float)extent_.height - sy);
    const bool prepassed = prepass_ && info.items && sw >= 1.f && sh >= 1.f;
    velocityDrawn_ = prepassed && velocityOn_;
    if (prepassed) {
        recordPrepass(cb, scene, sceneBatches_, gpuCulled_, {{(int32_t)sx, (int32_t)sy}, {(uint32_t)sw, (uint32_t)sh}});
        timestamp(cb, "Depth prepass");
    }
    if (fogOn_) {
        const bool readDepth = prepassed && depthSampleView_;
        if (readDepth)
            imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        recordFog(cb, scene, readDepth, {{(int32_t)sx, (int32_t)sy}, {(uint32_t)sw, (uint32_t)sh}});
        if (readDepth)
            imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, depthStages,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        timestamp(cb, "Volumetric fog");
    }
    if (prepassed) recordTrace(cb, scene);   // (reflections and GI at half size: renderer_trace.cpp)

    // ---- Pass 1: 3D scene (MSAA, resolved into the HDR image)
    Vec3 sky = srgbToLinear(info.sky);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.clearValue.color = {{sky.x, sky.y, sky.z, 1.f}};
    // (water: the pass stops after the opaque things to copy the picture for its refraction, then goes on)
    const bool splitForWater = refractOn_ && info.items && !transparentBatches_.empty();
    if (colorMsaa_.image) {
        color.imageView = colorMsaa_.view;
        color.storeOp = splitForWater ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = hdr_.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        color.imageView = hdr_.view;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = prepassed ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;   // (the prepass's)
    depthAtt.storeOp = aoSupported_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;   // (AO reads it)
    depthAtt.clearValue.depthStencil = {1.f, 0};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    vkCmdBeginRendering(cb, &ri);

    // Clamp the editor's viewport rectangle to the framebuffer (the scene's: smaller with a render scale)
    float vx = sx, vy = sy, vw = sw, vh = sh;
    if (info.items && vw >= 1.f && vh >= 1.f) {
        VkViewport viewport{vx, vy, vw, vh, 0.f, 1.f};
        VkRect2D scissor{{(int32_t)vx, (int32_t)vy}, {(uint32_t)vw, (uint32_t)vh}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        VkDescriptorSet boundTexture = VK_NULL_HANDLE;
        auto draw = [&](const Batch& b) {
            if (b.texture != boundTexture) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
                boundTexture = b.texture;
            }
            drawBatch(cb, b);
        };
        drawOpaque(cb, scene, sceneBatches_, gpuCulled_, false);
        if (gpuCulled_) stats_.triangles += gpuTriangles_;   // (a frame or two old)

        // The sky, wherever nothing was drawn
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdDraw(cb, 3, 1, 0, 0);

        if (splitForWater) {   // water's refraction: the opaque picture and its distances, then on with the rest
            vkCmdEndRendering(cb);
            const VkPipelineStageFlags fragStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            const VkImageLayout readable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, readable, colorStage,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage, VK_ACCESS_SHADER_READ_BIT);
            imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         fragStage, VK_ACCESS_SHADER_READ_BIT);
            mipBarrier(cb, refraction_.image, 0, 1, readable, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, fragStage, 0, colorStage,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = refraction_.view;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo copy{VK_STRUCTURE_TYPE_RENDERING_INFO};
            copy.renderArea = {{0, 0}, extent_};
            copy.layerCount = 1;
            copy.colorAttachmentCount = 1;
            copy.pColorAttachments = &att;
            vkCmdBeginRendering(cb, &copy);
            VkViewport full{0.f, 0.f, (float)extent_.width, (float)extent_.height, 0.f, 1.f};
            VkRect2D all{{0, 0}, extent_};
            vkCmdSetViewport(cb, 0, 1, &full);
            vkCmdSetScissor(cb, 0, 1, &all);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrCapturePipeline_);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &ssrSet_, 0, nullptr);
            float push[8] = {scene.zNear, scene.zFar, 0.f, 0.f, vx, vy, 0.f, 0.f};   // (scale 0: pixel for pixel)
            vkCmdPushConstants(cb, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            mipBarrier(cb, refraction_.image, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, readable, colorStage,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, fragStage, VK_ACCESS_SHADER_READ_BIT);
            imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, readable, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, fragStage, 0,
                         colorStage, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, fragStage, 0, depthStages,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            // (the scene pass again, keeping what's drawn)
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (colorMsaa_.image) color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            vkCmdBeginRendering(cb, &ri);
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            const uint32_t slot = 0;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descSets_[frame_], 1, &slot);
            boundTexture = VK_NULL_HANDLE;
            timestamp(cb, "Scene: opaque (water copies it)");
        }

        // See-through objects, back to front
        if (!transparentBatches_.empty()) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentPipeline_);
            for (const Batch& b : transparentBatches_) {
                draw(b);
                stats_.drawCalls++;
            }
        }

        // Particles: see-through ones (sorted back to front), then the additive ones
        if (particleAlpha_) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAlphaPipeline_);
            vkCmdDraw(cb, 6, particleAlpha_, 0, 0);
        }
        if (particleAdditive_) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAdditivePipeline_);
            vkCmdDraw(cb, 6, particleAdditive_, 0, particleAlpha_);
        }

        for (const Batch& b : outlineBatches_) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, stencilPipeline_);
            draw(b);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline_);
            draw(b);
        }
    }
    vkCmdEndRendering(cb);
    if (aoSupported_) {   // the depth, for ambient occlusion and the occlusion culling pyramid
        imageBarrier(cb, depth_.image, dsAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStages,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (info.items) recordHiz(cb, scene);
    }

    timestamp(cb, "Scene");
    // Blended with the frames before (renderer_taa.cpp); drawn smaller: brought up to full size there too (without
    // temporal AA: a sharpened upscale, no history)
    if ((aa_ == Antialiasing::Temporal || scaleNow_ < 0.999f) && info.items && taa_.img[0].image) {
        recordTaa(cb, info, taa_, scaleNow_);
        timestamp(cb, aa_ == Antialiasing::Temporal ? "Temporal AA" : "Upscale");
    }
    // ---- The UI in a layer of its own (HDR output): blended in by the composite, in linear light
    if (uiLayered_) {
        imageBarrier(cb, uiLayer_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo layer{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        layer.imageView = uiLayer_.view;
        layer.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        layer.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // (transparent: premultiplied, as ImGui's blending leaves it)
        layer.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo lri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        lri.renderArea = {{0, 0}, extent_};
        lri.layerCount = 1;
        lri.colorAttachmentCount = 1;
        lri.pColorAttachments = &layer;
        vkCmdBeginRendering(cb, &lri);
        if (ui) ImGui_ImplVulkan_RenderDrawData(ui, cb);
        vkCmdEndRendering(cb);
        imageBarrier(cb, uiLayer_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    // ---- Bloom and the composite onto the swapchain image
    recordPost(cb, imageIndex, info);
    timestamp(cb, "AO, bloom, composite");

    // ---- Pass 3: ImGui directly on the swapchain image (unless it's in the layer)
    imageBarrier(cb, swapImages_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, colorStage,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (ui && !uiLayered_) {
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
    timestamp(cb, "UI");

    const bool hdrShot = info.items && screenshotPath_.size() > 4 && screenshotPath_.compare(screenshotPath_.size() - 4, 4, ".hdr") == 0;
    if (hdrShot) {
        // An HDR screenshot: the scene's light before tone mapping (the HDR image, half floats), saved after the frame
        VkDeviceSize size = (VkDeviceSize)extent_.width * extent_.height * 8;
        if (!readback_.buffer || readback_.size < size) {
            if (readback_.buffer) destroyBuffer(readback_);
            readback_ = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            readback_.size = size;
        }
        imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cb, hdr_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
        imageBarrier(cb, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        screenshotExposure_ = info.exposure;
    }
    hdrShotRecorded_ = hdrShot;
    if (!screenshotPath_.empty() && canReadSwapchain_ && !hdrShot) {
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

void Renderer::drawOpaque(VkCommandBuffer cb, const SceneRenderInfo& info, const std::vector<Batch>& batches, bool culled,
                          bool depthOnly) {
    VkPipeline pipeline = depthOnly ? (velocityOn_ ? prepassVelocityPipeline_ : prepassPipeline_)
                                    : prepass_ ? sceneEarlyPipeline_ : scenePipeline_;
    VkPipeline plants = depthOnly ? (velocityOn_ ? prepassFoliageVelocityPipeline_ : prepassFoliagePipeline_)
                                  : prepass_ ? foliageEarlyPipeline_ : foliagePipeline_;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (size_t i = 0; i < batches.size(); ++i) {
        const Batch& b = batches[i];
        if (b.texture != bound) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1, &b.texture, 0, nullptr);
            bound = b.texture;
        }
        if (culled) {   // as many instances as the culling pass found visible
            size_t levels = 1 + meshes_->get(b.mesh).lods.size();   // (one draw per level it has)
            for (size_t k = 0; k < levels && k < 3; ++k)
                vkCmdDrawIndexedIndirect(cb, cullCommands_[frame_].buffer, (i * 3 + k) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                         sizeof(VkDrawIndexedIndirectCommand));
        } else {
            drawBatch(cb, b);
            if (!depthOnly) stats_.triangles += rangeOf(b).indexCount / 3 * b.count;
        }
        if (!depthOnly) stats_.drawCalls++;
    }
    drawPlants(cb, info, plants);
}

void Renderer::recordPrepass(VkCommandBuffer cb, const SceneRenderInfo& info, const std::vector<Batch>& batches, bool culled,
                             VkRect2D area) {
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depth_.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // (the lit pass loads it)
    depthAtt.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depthAtt;
    ri.pStencilAttachment = &depthAtt;
    // With temporal AA, each pixel's motion too (where nothing's drawn: kNoVelocity, so the history is found by
    // the depth: the sky)
    VkRenderingAttachmentInfo velocity{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, outputs[2];
    const VkPipelineStageFlags colorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (velocityOn_) {
        mipBarrier(cb, velocity_.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        velocity.imageView = velocity_.view;
        velocity.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        velocity.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        velocity.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        velocity.clearValue.color = {{kNoVelocity, 0.f, 0.f, 0.f}};
        mipBarrier(cb, gbuffer_.image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        outputs[0] = velocity;
        outputs[1] = velocity;
        outputs[1].imageView = gbuffer_.view;
        outputs[1].clearValue.color = {{0.f, 0.f, 0.f, 0.f}};   // (w = 0: nothing drawn)
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = outputs;
    }
    vkCmdBeginRendering(cb, &ri);
    VkViewport viewport{(float)area.offset.x, (float)area.offset.y, (float)area.extent.width, (float)area.extent.height, 0.f, 1.f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &area);
    drawOpaque(cb, info, batches, culled, true);
    vkCmdEndRendering(cb);
    if (velocityOn_)
        for (VkImage image : {velocity_.image, gbuffer_.image})
            mipBarrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       colorStage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
}


void Renderer::saveScreenshot() {
    std::string path = std::move(screenshotPath_);
    screenshotPath_.clear();
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".hdr") == 0) {   // (the HDR image: recordCommands)
        if (!hdrShotRecorded_) {
            std::fprintf(stderr, "screenshot: no scene drawn for an HDR screenshot\n");
            screenshotResult_ = -1;
            return;
        }
        vkQueueWaitIdle(queue_);
        const size_t n = (size_t)extent_.width * extent_.height;
        const auto* half = static_cast<const uint16_t*>(readback_.mapped);
        std::vector<float> rgb(n * 3);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c) rgb[i * 3 + (size_t)c] = halfToFloat(half[i * 4 + (size_t)c]) * screenshotExposure_;
        bool ok = writeRadianceHdr(path, extent_.width, extent_.height, rgb.data());
        std::printf(ok ? "Saved HDR screenshot %s (%ux%u)\n" : "screenshot: cannot write %s\n", path.c_str(), extent_.width,
                    extent_.height);
        screenshotResult_ = ok ? 1 : -1;
        return;
    }
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


// The frame's camera, lights and look (and, with `shadows`, where the shadow cameras go this frame)
void Renderer::fillFrameUBO(const SceneRenderInfo& info, FrameUBO& u, bool shadows) {
    // Lights: the first directional light is the sun (a default one if there's none), then the point and spot
    // lights nearest the camera
    LightInfo sun;
    sun.directional = true;
    sun.direction = normalize({-0.45f, -1.f, -0.3f});
    sun.color = {1.f, 0.95f, 0.85f};
    std::vector<const LightInfo*> points;
    bool haveSun = false;
    if (info.lights)
        for (const LightInfo& l : *info.lights) {
            if (l.directional && !haveSun) { sun = l; haveSun = true; }
            else if (!l.directional && l.intensity > 0.f && l.range > 0.f) points.push_back(&l);
        }
    // Nearest first, counting from the edge of their reach (a big light a little further away matters more)
    auto reach = [&](const LightInfo* l) { return std::max(length(l->position - info.camPos) - l->range, 0.f); };
    std::stable_sort(points.begin(), points.end(), [&](const LightInfo* a, const LightInfo* b) {
        float ra = reach(a), rb = reach(b);
        if (ra != rb) return ra < rb;
        return length(a->position - info.camPos) < length(b->position - info.camPos);
    });
    if (points.size() > (size_t)kMaxLights) points.resize(kMaxLights);
    // The sky's colours: the scene's, or the air's (the sun turned and coloured by it: before its shadows)
    Vec3 fog = srgbToLinear(info.sky), zenith = srgbToLinear(info.zenith), ground = srgbToLinear(info.ground);
    if (info.physicalSky) applyAtmosphere(info, sun, fog, zenith, ground, u);
    if (shadows) {
        frameShadows_ = sun.castShadows && sun.intensity > 0.f;
        skyOn_ = info.physicalSky;
    }

    u.viewProj = info.viewProj;
    std::vector<int> lightShadow(points.size(), -1);
    if (shadows) {   // water: refraction (with something to read the depth and copy the picture) and the caustics' boxes
        int boxes = 0;
        bool anyWater = false;
        if (info.items)
            for (const DrawItem& d : *info.items) {
                if (!d.water) continue;
                anyWater = true;
                if (boxes >= 4) continue;
                const Mat4& m = d.model;   // (its grid spans -0.5..0.5 under this: the rectangle's corners)
                float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
                for (int c = 0; c < 4; ++c) {
                    Vec3 p = transformPoint(m, {c & 1 ? 0.5f : -0.5f, 0.f, c & 2 ? 0.5f : -0.5f});
                    lo[0] = std::min(lo[0], p.x), lo[1] = std::min(lo[1], p.z);
                    hi[0] = std::max(hi[0], p.x), hi[1] = std::max(hi[1], p.z);
                }
                u.waterBox[boxes][0] = lo[0], u.waterBox[boxes][1] = lo[1], u.waterBox[boxes][2] = hi[0], u.waterBox[boxes][3] = hi[1];
                u.waterLevel[boxes][0] = m.m[13];
                u.waterLevel[boxes][1] = d.waterDepth;
                ++boxes;
            }
        refractOn_ = anyWater && aoSupported_ && ssrCapturePipeline_ && refraction_.image;
        u.waterInfo[0] = refractOn_ ? 1.f : 0.f;
        u.waterInfo[1] = (float)boxes;
    }
    if (shadows) {   // motion vectors: the camera now and last frame, unjittered (the prepass writes them with temporal AA)
        // (half-size traces: no MSAA, the prepass, a depth to read, last frame's picture, full size)
        // (and contact shadows, which need only this frame's depth)
        const bool contact = info.contactShadows && quality_ != Quality::Low;
        const bool traceWanted = ((captureOn(info) && ssrValid_) || contact) && aoSupported_ && ssrTracePipeline_ &&
                                 scaleNow_ >= 0.999f;
        velocityOn_ = (aa_ == Antialiasing::Temporal || traceWanted) && prepass_ && info.items && prepassVelocityPipeline_ &&
                      velocity_.image;
        traceOn_ = traceWanted && velocityOn_;
        u.trace[0] = traceOn_ ? 1.f : 0.f;
        u.trace[1] = traceOn_ && traceHistory_ ? 1.f : 0.f;
        u.trace[2] = (float)(traceIndex_++ % 4);
        u.trace[3] = traceOn_ && contact ? 1.f : 0.f;
        traceHistory_ = traceOn_;
        u.motionViewProj = info.viewProj;
        u.motionPrevViewProj = motionPrevValid_ ? motionPrevViewProj_ : info.viewProj;
        u.motion[0] = std::clamp(info.frameSeconds, 0.f, 0.25f);
        u.motion[1] = velocityOn_ ? 1.f : 0.f;
        motionPrevViewProj_ = info.viewProj;
        motionPrevValid_ = true;
    }
    if (shadows) {
        shadowLayers_ = computeShadows(info, normalize(sun.direction), frameShadows_, points, u, lightShadow);
        for (uint32_t i = 0; i < kShadowLayers; ++i) shadowViewProj_[i] = u.shadowViewProj[i];
    }
    set4(u.camPos, info.camPos, 1.f);
    set4(u.fogColor, fog, info.fogDensity);
    u.viewport[0] = info.viewportW;
    u.viewport[1] = info.viewportH;
    u.viewport[2] = 2.5f;
    set4(u.sunDir, normalize(sun.direction), shadows && frameShadows_ ? 1.f : 0.f);
    set4(u.sunColor, info.physicalSky ? skySunHere_ * sun.intensity : srgbToLinear(sun.color) * sun.intensity, 0.f);
    set4(u.ambient, {info.ambient, info.ambient, info.ambient}, (float)points.size());
    writeLights(info, points, lightShadow, u, shadows);
    if (shadows) writeDecals(info, u);   // (and the weather; binned into the lights' clusters)
    else u.weather[0] = info.wetness, u.weather[1] = info.puddles, u.weather[2] = info.rain;
    set4(u.zenith, zenith, 0.f);
    set4(u.ground, ground, 0.f);
    set4(u.envParams, {envId_ != 0 ? 1.f : 0.f, info.skyIntensity, info.skyRotation}, 0.f);
    if (!info.physicalSky || envId_ != 0)   // (the physical sky's own harmonics, else)
        for (int k = 0; k < 9; ++k) set4(u.sh[k], {envSh_[k][0], envSh_[k][1], envSh_[k][2]}, 0.f);
    if (envId_ != 0) u.atmosphere[1] = 0.f;   // (a sky image lights the scene by its own)
    {   // the camera's axes, for billboards and the sky (no roll: up is as close to the world's up as possible)
        Vec3 f = normalize(info.camForward);
        Vec3 right = std::fabs(f.y) > 0.999f ? Vec3{1, 0, 0} : normalize(cross(f, {0, 1, 0}));
        set4(u.camRight, right, 0.f);
        set4(u.camUp, cross(right, f), 0.f);
        set4(u.camForward, f, std::tan(info.fovY * 0.5f));
    }
    static const auto started = std::chrono::steady_clock::now();
    u.time[0] = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
    u.time[1] = (float)(frameCounter_ % 16);   // (texture streaming's reporting pixels move each frame)
}

float Renderer::nextDynamicScale(float now, float gpuMs, float budgetMs, float maxScale) {
    if (gpuMs <= 0.f || budgetMs <= 0.f) return std::clamp(now, 0.5f, maxScale);
    // Pixels go as the scale squared: the scale that would take the frame to 90 % of its budget, gone towards in steps of
    // 5 % (half the way's worth of them, up to four), down as soon as it's a step too slow, up only past a whole step
    // (no going back and forth around one)
    float want = std::clamp(now * std::sqrt(0.9f * budgetMs / gpuMs), 0.5f, maxScale);
    float gap = want - now;
    if (gap > -0.025f && gap < 0.05f) return std::clamp(now, 0.5f, maxScale);
    float steps = std::clamp(std::round(std::fabs(gap) / 0.05f * 0.5f), 1.f, 4.f);
    float next = std::round((now + (gap < 0.f ? -steps : steps) * 0.05f) * 20.f) / 20.f;
    return std::clamp(gap < 0.f ? std::max(next, want - 0.025f) : std::min(next, want), 0.5f, maxScale);
}

void Renderer::setRenderScale(float scale) {
    if (const char* v = std::getenv("VKWORLD_RENDER_SCALE")) scale = (float)std::atof(v);
    scale = std::clamp(scale, 0.5f, 1.f);
    bool was = upscaling();
    renderScaleSet_ = scale;
    if (!dynamicResolution_) scaleNow_ = scale;
    else scaleNow_ = std::min(scaleNow_, scale);
    if (was != upscaling() && device_) {   // (the upscaling pass's images come and go)
        vkDeviceWaitIdle(device_);
        recreateSwapchain();
    }
}

void Renderer::setDynamicResolution(bool on, float targetFps) {
    targetFps_ = std::clamp(targetFps, 15.f, 360.f);
    if (on == dynamicResolution_) return;
    bool was = upscaling();
    dynamicResolution_ = on;
    scaleNow_ = renderScaleSet_;
    if (was != upscaling() && device_) {
        vkDeviceWaitIdle(device_);
        recreateSwapchain();
    }
}

void Renderer::render(const SceneRenderInfo& info, ImDrawData* ui) {
    if (width_ == 0 || height_ == 0) return;
    PROFILE_SCOPE("Render (CPU)");
    if (resized_) recreateSwapchain();
    // Render scale: this frame's (dynamic: from the last measured frame, held a while after each change), and the
    // scene's viewport at it
    if (dynamicResolution_ && --scaleHold_ <= 0) {
        float next = nextDynamicScale(scaleNow_, gpuFrameMs_, 1000.f / targetFps_, renderScaleSet_);
        if (next != scaleNow_) scaleHold_ = 30;
        scaleNow_ = next;
    }
    if (!supports(Antialiasing::Temporal)) scaleNow_ = 1.f;   // (no depth to read: no upscaling either)
    SceneRenderInfo scene = info;
    if (scaleNow_ < 0.999f) {
        scene.viewportW = std::max(1.f, std::floor(info.viewportW * scaleNow_));
        scene.viewportH = std::max(1.f, std::floor(info.viewportH * scaleNow_));
    }
    if (meshes_->version != uploadedMeshVersion_) uploadMeshes();
    if (info.antialiasing >= 0 && info.antialiasing != (int)aa_) setAntialiasing((Antialiasing)info.antialiasing);
    reuploadChangedTextures();   // (before any of this frame's material sets are made: it may remake them all)
    uploadEnvironment(info.environment);
    updateCookies(info);
    updateProbes(info);
    if (occlusion_ && gpuCulling_) ensureHiz(scene);   // (sized to the viewport the scene is drawn in)

    {
        PROFILE_SCOPE("Wait for the GPU");
        VK_CHECK(vkWaitForFences(device_, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX));
    }
    readGpuTimes();   // that frame's pass times
    readCullStats();   // ... and what its culling pass let through
    writeProbeDescriptors();   // (its set is free now)
    for (Buffer& b : retired_[frame_]) destroyBuffer(b);   // staging for uploads that frame made: done now
    retired_[frame_].clear();

    uint32_t imageIndex = 0;
    VkResult r;
    {
        PROFILE_SCOPE("Acquire image");
        r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_[frame_], VK_NULL_HANDLE, &imageIndex);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VK_CHECK(r);
    VK_CHECK(vkResetFences(device_, 1, &inFlight_[frame_]));

    // The extra views' frame data first (their lights go into the shared buffer: this view's, after, are the ones
    // kept), then this view's; the views take its shadows
    size_t viewCount = info.views ? std::min(info.views->size(), (size_t)kMaxViews) : 0;
    std::vector<FrameUBO> viewUbos(viewCount);
    for (size_t v = 0; v < viewCount; ++v) {
        fillFrameUBO((*info.views)[v], viewUbos[v], false);
        setProbeData(viewUbos[v]);
    }
    if (materialSetsMade_ > 3584) {   // (stale material sets from streamed textures: before any batch takes one)
        vkDeviceWaitIdle(device_);
        vkResetDescriptorPool(device_, materialPool_, 0);
        materialSets_.clear();
        terrainSets_.clear();
        materialSetsMade_ = 0;
    }
    planGi(info);   // (probes' faces: before this view too)
    FrameUBO u{};
    fillFrameUBO(scene, u, true);
    setProbeData(u);
    finishGi(u);
    // Volumetric fog: the main view's (only with something to draw and its shadows worked out)
    fogOn_ = info.volumetricFog && info.items && fogInjectPipeline_ && info.fogDensity > 0.f;
    if (fogOn_) {
        const float far = std::clamp(fogFar_, 10.f, 1000.f), near = std::clamp(fogNear_ * 0.5f, 0.5f, far * 0.25f);
        u.fogParams[3] = info.fogHeightFalloff;
        u.fogParams[2] = std::clamp(info.fogAnisotropy, -0.95f, 0.95f);
        u.fogParams[1] = info.fogLight;
        u.fogParams[0] = 1.f;
        u.fogRange[0] = near;
        u.fogRange[1] = 64.f / std::log(far / near);
        u.fogRange[2] = far;
        // Last frame's, for the cells' history (none the first frame, or after a frame without fog)
        u.fogPrevViewProj = fogPrevViewProj_;
        set4(u.fogPrevCam, fogPrevCam_, 0.f);
        set4(u.fogPrevForward, fogPrevForward_, 0.f);
        u.fogPrevRange[0] = fogPrevRange_[0];
        u.fogPrevRange[1] = fogPrevRange_[1];
        u.fogPrevRange[3] = fogHistory_ ? 1.f : 0.f;
        fogPrevViewProj_ = info.viewProj;
        fogPrevCam_ = info.camPos;
        fogPrevForward_ = normalize(info.camForward);
        fogPrevRange_[0] = u.fogRange[0];
        fogPrevRange_[1] = u.fogRange[1];
    }
    fogHistory_ = fogOn_;
    if (captureOn(info) && ssrValid_) {   // the last frame's camera, for rays into its picture
        u.prevViewProj = ssrViewProj_;
        u.prevCamPos[0] = ssrCamPos_.x, u.prevCamPos[1] = ssrCamPos_.y, u.prevCamPos[2] = ssrCamPos_.z;
        u.prevCamPos[3] = reflectionsOn(info) ? 1.f : 0.f;
        u.prevCamForward[0] = ssrCamForward_.x, u.prevCamForward[1] = ssrCamForward_.y, u.prevCamForward[2] = ssrCamForward_.z;
        u.prevCamForward[3] = ssgiOn(info) ? std::clamp(info.ssgiIntensity, 0.f, 1.f) : 0.f;
        std::memcpy(u.prevViewport, ssrViewport_, sizeof(ssrViewport_));
    }
    ssrValid_ = captureOn(info) && info.items && ssrCapturePipeline_;   // (this frame's picture is captured)
    ssrViewProj_ = info.viewProj;
    ssrCamPos_ = info.camPos;
    ssrCamForward_ = info.camForward;
    ssrViewport_[0] = info.viewportX, ssrViewport_[1] = info.viewportY, ssrViewport_[2] = info.viewportW, ssrViewport_[3] = info.viewportH;
    uploadPlants(info);
    for (size_t v = 0; v < viewCount; ++v) {
        FrameUBO& uv = viewUbos[v];
        uv.ambient[3] = u.ambient[3];   // (the lights buffer holds this view's)
        std::memcpy(uv.giOrigin, u.giOrigin, sizeof(u.giOrigin));
        std::memcpy(uv.giDims, u.giDims, sizeof(u.giDims));
        std::memcpy(uv.giCell, u.giCell, sizeof(u.giCell));
        std::memcpy(uv.shadowViewProj, u.shadowViewProj, sizeof(u.shadowViewProj));
        std::memcpy(uv.shadowRect, u.shadowRect, sizeof(u.shadowRect));
        std::memcpy(uv.cascadeSplits, u.cascadeSplits, sizeof(u.cascadeSplits));
        uv.viewport[3] = 0.f;
        if (viewShadowed_[v]) {   // its own cascades (atlas tiles) in the cascades' places
            const uint32_t first = kViewCascades + (uint32_t)v * kCascades;
            std::memcpy(uv.shadowViewProj, u.shadowViewProj + first, sizeof(Mat4) * kCascades);
            std::memcpy(uv.shadowRect, u.shadowRect + first, sizeof(u.shadowRect[0]) * kCascades);
            std::memcpy(uv.cascadeSplits, viewSplits_[v], sizeof(uv.cascadeSplits));
            uv.viewport[3] = 1.f;
        }
        uv.sunDir[3] = u.sunDir[3];
        uv.weather[3] = u.weather[3];   // (the decals: the main view's)
        uv.waterInfo[1] = u.waterInfo[1];   // (the water's boxes, for caustics; refraction: the main view's alone)
        std::memcpy(uv.waterBox, u.waterBox, sizeof(u.waterBox));
        std::memcpy(uv.waterLevel, u.waterLevel, sizeof(u.waterLevel));
        std::memcpy(static_cast<uint8_t*>(ubo_[frame_].mapped) + uboStride_ * (1 + v), &uv, sizeof(uv));
    }
    if (aa_ == Antialiasing::Temporal) {   // jittered, a different sub-pixel offset every frame
        jitterIndex_ = jitterIndex_ % 8 + 1;
        jitterView(scene, u, taa_, &info);
        for (size_t v = 0; v < viewCount; ++v) {   // (the views' too, into their slots)
            jitterView((*info.views)[v], viewUbos[v], viewImages_[v].taa);
            std::memcpy(static_cast<uint8_t*>(ubo_[frame_].mapped) + uboStride_ * (1 + v), &viewUbos[v], sizeof(FrameUBO));
        }
    }
    u.invViewProj = inverse(u.viewProj);   // (jittered, as drawn: the half-size traces find points by the depth)
    std::memcpy(ubo_[frame_].mapped, &u, sizeof(u));
    {
        PROFILE_SCOPE("Instances");
        prepareMotion(info);   // (before the instances and joints: where each was last frame)
        prepareInstances(info);
        uploadJoints(info);
        uploadParticles(info);
    }

    VK_CHECK(vkResetCommandBuffer(cmd_[frame_], 0));
    {
        PROFILE_SCOPE("Record commands");
        recordCommands(cmd_[frame_], imageIndex, info, scene, ui);
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &imageAvailable_[frame_];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_[frame_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone_[imageIndex];
    {
        PROFILE_SCOPE("Submit");
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, inFlight_[frame_]));
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    if (!screenshotPath_.empty()) saveScreenshot();
    {
        PROFILE_SCOPE("Present");
        r = vkQueuePresentKHR(queue_, &pi);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || resized_) recreateSwapchain();
    else VK_CHECK(r);

    frame_ = (frame_ + 1) % MAX_FRAMES;
}

void Renderer::shutdown() {
    vkDeviceWaitIdle(device_);
    for (ViewImage& v : viewImages_) destroyViewImage(v);   // (their textures: before the ImGui backend goes)
    destroyGi();
    destroyFog();
    destroySky();
    if (imguiInitialized_) ImGui_ImplVulkan_Shutdown();
    destroySwapchainResources();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    destroySampledPipelines();
    for (VkPipeline p : {shadowPipeline_, shadowCutoutPipeline_, bloomDownPipeline_, bloomUpPipeline_, compositePipeline_,
                         compositeViewPipeline_,
                         taaPipeline_, taaUpscalePipeline_, lumPipeline_, adaptPipeline_, ssrTracePipeline_, giTracePipeline_})
        vkDestroyPipeline(device_, p, nullptr);
    vkDestroyPipelineLayout(device_, taaLayout_, nullptr);
    destroyPostImages();
    vkDestroyPipelineLayout(device_, postLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, postSetLayout_, nullptr);
    vkDestroyDescriptorPool(device_, postPool_, nullptr);
    vkDestroySampler(device_, postSampler_, nullptr);
    vkDestroySampler(device_, lumSampler_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descPool_, nullptr);
    for (GpuTexture& t : gpuTextures_) destroyTexture(t);
    destroyTexture(whiteTexture_);
    destroyTexture(flatNormal_);
    vkDestroyDescriptorPool(device_, materialPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, materialSetLayout_, nullptr);
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroySampler(device_, nearestSampler_, nullptr);
    destroyEnvironmentResources();
    destroyProbes();
    destroyCulling();
    vkDestroySampler(device_, shadowSampler_, nullptr);
    vkDestroySampler(device_, shadowCompareSampler_, nullptr);
    destroyShadowArray(cascades_);
    destroyShadowArray(atlas_);
    destroyImage(cookies_);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        destroyBuffer(lights_[i]);
        destroyBuffer(decalBuffers_[i]);
        destroyBuffer(decalClusters_[i]);
        destroyBuffer(clusters_[i]);
        destroyBuffer(plantBuffers_[i]);
        if (feedback_[i].buffer) destroyBuffer(feedback_[i]);
        for (GpuTexture& t : retiredTextures_[i]) destroyTexture(t);
        retiredTextures_[i].clear();
    }
    destroyBuffer(vertexBuffer_);
    destroyBuffer(skinBuffer_);
    destroyBuffer(indexBuffer_);
    if (readback_.buffer) destroyBuffer(readback_);
    for (std::vector<Buffer>& list : retired_)
        for (Buffer& b : list) destroyBuffer(b);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        destroyBuffer(ubo_[i]);
        if (instanceStaging_[i].buffer) destroyBuffer(instanceStaging_[i]);
        if (i == 0 && instanceStore_.buffer) destroyBuffer(instanceStore_);
        destroyBuffer(joints_[i]);
        destroyBuffer(particles_[i]);
        vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        vkDestroyFence(device_, inFlight_[i], nullptr);
        if (timestamps_[i]) vkDestroyQueryPool(device_, timestamps_[i], nullptr);
    }
    vkDestroyCommandPool(device_, cmdPool_, nullptr);
    savePipelineCache();
    if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) {
        auto destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(instance_, messenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
}
