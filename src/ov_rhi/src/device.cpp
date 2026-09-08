#define OV_LOG_CATEGORY "rhi"

#include "vulkan_common.hpp"

#include "ov/base/log.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ov::rhi {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        OV_LOG_ERROR("validation: {}", data->pMessage);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        OV_LOG_WARN("validation: {}", data->pMessage);
    }
    return VK_FALSE;
}

[[nodiscard]] bool has_extension(const std::vector<VkExtensionProperties>& available,
                                 std::string_view                          name) {
    return std::ranges::any_of(available, [name](const VkExtensionProperties& extension) {
        return name == extension.extensionName;
    });
}

[[nodiscard]] bool has_layer(const std::vector<VkLayerProperties>& available,
                             std::string_view                      name) {
    return std::ranges::any_of(
        available, [name](const VkLayerProperties& layer) { return name == layer.layerName; });
}

/// Rank a physical device.
///
/// On macOS the loader offers more than one driver for the same GPU — MoltenVK
/// and Mesa's KosmicKrisp both enumerate an Apple M2 here — and picking
/// whichever came first makes the renderer's behaviour depend on the order a
/// driver package happened to install its ICD. MoltenVK is the one this project
/// targets and the one risk R6 is written about, so it is preferred explicitly.
[[nodiscard]] i32 score_device(VkPhysicalDevice device) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &driver;
    vkGetPhysicalDeviceProperties2(device, &properties);

    i32 score = 0;
    if (driver.driverID == VK_DRIVER_ID_MOLTENVK) {
        score += 1000;
    }
    if (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 500;
    } else if (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 100;
    }
    return score;
}

}  // namespace

std::string_view to_string(RhiError error) noexcept {
    switch (error) {
        case RhiError::NoVulkan: return "no Vulkan loader or driver";
        case RhiError::NoDevice: return "no usable Vulkan device";
        case RhiError::NoSurface: return "could not create a window surface";
        case RhiError::OutOfMemory: return "out of memory";
        case RhiError::FileNotFound: return "file not found";
        case RhiError::BadShader: return "shader rejected by the driver";
        case RhiError::SwapchainOutOfDate: return "swapchain out of date";
        case RhiError::DeviceLost: return "device lost";
        case RhiError::InvalidArgument: return "invalid argument";
    }
    return "unknown RHI error";
}

bool is_depth_format(Format format) noexcept {
    return format == Format::Depth32Float || format == Format::Depth24UnormStencil8;
}

u32 format_size(Format format) noexcept {
    switch (format) {
        case Format::Undefined: return 0;
        case Format::R8Unorm: return 1;
        case Format::R16Uint: return 2;
        case Format::Rgba8Unorm:
        case Format::Rgba8Srgb:
        case Format::Bgra8Unorm:
        case Format::Bgra8Srgb:
        case Format::R32Uint:
        case Format::Depth32Float:
        case Format::Depth24UnormStencil8: return 4;
        case Format::Rg32Float: return 8;
        case Format::Rgb32Float: return 12;
        case Format::Rgba32Float: return 16;
    }
    return 0;
}

VkFormat to_vulkan(Format format) noexcept {
    switch (format) {
        case Format::Undefined: return VK_FORMAT_UNDEFINED;
        case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
        case Format::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::Bgra8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R16Uint: return VK_FORMAT_R16_UINT;
        case Format::R32Uint: return VK_FORMAT_R32_UINT;
        case Format::Rg32Float: return VK_FORMAT_R32G32_SFLOAT;
        case Format::Rgb32Float: return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::Depth32Float: return VK_FORMAT_D32_SFLOAT;
        case Format::Depth24UnormStencil8: return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

Format from_vulkan(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return Format::R8Unorm;
        case VK_FORMAT_R8G8B8A8_UNORM: return Format::Rgba8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB: return Format::Rgba8Srgb;
        case VK_FORMAT_B8G8R8A8_UNORM: return Format::Bgra8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB: return Format::Bgra8Srgb;
        case VK_FORMAT_D32_SFLOAT: return Format::Depth32Float;
        case VK_FORMAT_D24_UNORM_S8_UINT: return Format::Depth24UnormStencil8;
        default: return Format::Undefined;
    }
}

StateBarrier barrier_for(ResourceState state) noexcept {
    switch (state) {
        case ResourceState::Undefined:
            return {VK_IMAGE_LAYOUT_UNDEFINED, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT};
        case ResourceState::ColourAttachment:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
        case ResourceState::DepthAttachment:
            return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT};
        case ResourceState::ShaderRead:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT};
        case ResourceState::TransferSource:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT};
        case ResourceState::TransferDest:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT};
        case ResourceState::Present:
            return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT};
    }
    return {};
}

// ── Construction ────────────────────────────────────────────────────────────

Device::Device() : impl_(std::make_unique<Impl>()) {}

Device::~Device() {
    if (impl_->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl_->device);
        impl_->save_pipeline_cache();

        impl_->pipelines.for_each([this](PipelineResource& resource) {
            vkDestroyPipeline(impl_->device, resource.pipeline, nullptr);
            vkDestroyPipelineLayout(impl_->device, resource.layout, nullptr);
            if (resource.set_layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(impl_->device, resource.set_layout, nullptr);
            }
        });
        impl_->samplers.for_each([this](SamplerResource& resource) {
            vkDestroySampler(impl_->device, resource.sampler, nullptr);
        });
        impl_->images.for_each([this](ImageResource& resource) {
            vkDestroyImageView(impl_->device, resource.view, nullptr);
            vmaDestroyImage(impl_->allocator, resource.image, resource.allocation);
        });
        impl_->buffers.for_each([this](BufferResource& resource) {
            vmaDestroyBuffer(impl_->allocator, resource.buffer, resource.allocation);
        });

        impl_->destroy_swapchain();
        if (impl_->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(impl_->device, impl_->swapchain, nullptr);
        }

        for (auto& frame : impl_->frames) {
            if (frame.command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(impl_->device, frame.command_pool, nullptr);
            }
            if (frame.descriptor_pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(impl_->device, frame.descriptor_pool, nullptr);
            }
            if (frame.image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(impl_->device, frame.image_available, nullptr);
            }
            if (frame.in_flight != VK_NULL_HANDLE) {
                vkDestroyFence(impl_->device, frame.in_flight, nullptr);
            }
        }
        if (impl_->transfer_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl_->device, impl_->transfer_pool, nullptr);
        }
        if (impl_->timestamp_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(impl_->device, impl_->timestamp_pool, nullptr);
        }
        if (impl_->pipeline_cache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(impl_->device, impl_->pipeline_cache, nullptr);
        }
        if (impl_->allocator != nullptr) {
            vmaDestroyAllocator(impl_->allocator);
        }
        vkDestroyDevice(impl_->device, nullptr);
    }

    if (impl_->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    }
    if (impl_->messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(impl_->instance, impl_->messenger, nullptr);
    }
    if (impl_->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(impl_->instance, nullptr);
    }
}

std::expected<std::unique_ptr<Device>, RhiError> Device::create(const DeviceDesc& desc) {
    if (volkInitialize() != VK_SUCCESS) {
        OV_LOG_ERROR("no Vulkan loader. Install the SDK from vulkan.lunarg.com.");
        return std::unexpected(RhiError::NoVulkan);
    }

    std::unique_ptr<Device> self(new Device());
    Impl&                   impl = *self->impl_;
    impl.desc                    = desc;

    // ── Instance ────────────────────────────────────────────────────────────
    u32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    u32 instance_extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr);
    std::vector<VkExtensionProperties> instance_extensions(instance_extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count,
                                           instance_extensions.data());

    std::vector<const char*> enabled_extensions;
    std::vector<const char*> enabled_layers;

    // MoltenVK is a portability driver: without this the loader hides it.
    VkInstanceCreateFlags instance_flags = 0;
    if (has_extension(instance_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    enabled_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    if (has_extension(instance_extensions, "VK_EXT_metal_surface")) {
        enabled_extensions.push_back("VK_EXT_metal_surface");
    }
    if (has_extension(instance_extensions,
                      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        enabled_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }

    const bool want_validation =
        desc.validation && has_layer(layers, "VK_LAYER_KHRONOS_validation") &&
        has_extension(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (want_validation) {
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else if (desc.validation) {
        OV_LOG_WARN("validation layers requested but not available");
    }

    const std::string application_name(desc.application_name);

    VkApplicationInfo application{};
    application.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName   = application_name.c_str();
    application.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    application.pEngineName        = "Ondes VOXEL";
    application.apiVersion         = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.flags                   = instance_flags;
    instance_info.pApplicationInfo        = &application;
    instance_info.enabledExtensionCount   = static_cast<u32>(enabled_extensions.size());
    instance_info.ppEnabledExtensionNames = enabled_extensions.data();
    instance_info.enabledLayerCount       = static_cast<u32>(enabled_layers.size());
    instance_info.ppEnabledLayerNames     = enabled_layers.data();

    if (vkCreateInstance(&instance_info, nullptr, &impl.instance) != VK_SUCCESS) {
        return std::unexpected(RhiError::NoVulkan);
    }
    volkLoadInstanceOnly(impl.instance);
    impl.info.validation_enabled = want_validation;

    if (want_validation) {
        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        messenger.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = debug_callback;
        vkCreateDebugUtilsMessengerEXT(impl.instance, &messenger, nullptr, &impl.messenger);
    }

    // ── Surface ─────────────────────────────────────────────────────────────
    if (desc.native_window != nullptr) {
        auto* window = static_cast<GLFWwindow*>(desc.native_window);
        if (glfwCreateWindowSurface(impl.instance, window, nullptr, &impl.surface) != VK_SUCCESS) {
            OV_LOG_ERROR("glfwCreateWindowSurface failed");
            return std::unexpected(RhiError::NoSurface);
        }
    }

    // ── Physical device ─────────────────────────────────────────────────────
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(impl.instance, &device_count, nullptr);
    if (device_count == 0) {
        return std::unexpected(RhiError::NoDevice);
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(impl.instance, &device_count, devices.data());

    i32 best = -1;
    for (VkPhysicalDevice candidate : devices) {
        const i32 score = score_device(candidate);
        if (score > best) {
            best          = score;
            impl.physical = candidate;
        }
    }
    if (impl.physical == VK_NULL_HANDLE) {
        return std::unexpected(RhiError::NoDevice);
    }

    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical, &family_count, families.data());

    bool found_family = false;
    for (u32 i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        if (impl.surface != VK_NULL_HANDLE) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(impl.physical, i, impl.surface, &present);
            if (present != VK_TRUE) {
                continue;
            }
        }
        impl.queue_family = i;
        found_family      = true;
        break;
    }
    if (!found_family) {
        return std::unexpected(RhiError::NoDevice);
    }

    // ── Logical device ──────────────────────────────────────────────────────
    u32 device_extension_count = 0;
    vkEnumerateDeviceExtensionProperties(impl.physical, nullptr, &device_extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(device_extension_count);
    vkEnumerateDeviceExtensionProperties(impl.physical, nullptr, &device_extension_count,
                                         device_extensions.data());

    std::vector<const char*> device_extension_names;
    if (impl.surface != VK_NULL_HANDLE) {
        device_extension_names.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    // Required by the spec whenever the driver advertises it, and MoltenVK
    // always does.
    if (has_extension(device_extensions, "VK_KHR_portability_subset")) {
        device_extension_names.push_back("VK_KHR_portability_subset");
        impl.info.portability_subset = true;
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // `discard` in GLSL compiles to OpDemoteToHelperInvocation when targeting
    // Vulkan 1.3, and the cutout layer is built on discard. Without this the
    // driver rejects the shader module rather than the pipeline, which is a
    // confusing place to find out.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;

    const f32               priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = impl.queue_family;
    queue_info.queueCount       = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext                   = &features;
    device_info.queueCreateInfoCount    = 1;
    device_info.pQueueCreateInfos       = &queue_info;
    device_info.enabledExtensionCount   = static_cast<u32>(device_extension_names.size());
    device_info.ppEnabledExtensionNames = device_extension_names.data();

    if (vkCreateDevice(impl.physical, &device_info, nullptr, &impl.device) != VK_SUCCESS) {
        return std::unexpected(RhiError::NoDevice);
    }
    volkLoadDevice(impl.device);
    vkGetDeviceQueue(impl.device, impl.queue_family, 0, &impl.queue);

    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &driver;
    vkGetPhysicalDeviceProperties2(impl.physical, &properties);

    impl.info.name        = properties.properties.deviceName;
    impl.info.driver      = driver.driverName;
    impl.info.api_major   = VK_API_VERSION_MAJOR(properties.properties.apiVersion);
    impl.info.api_minor   = VK_API_VERSION_MINOR(properties.properties.apiVersion);
    impl.timestamp_period = static_cast<f64>(properties.properties.limits.timestampPeriod);

    OV_LOG_INFO("{} via {} (Vulkan {}.{}){}", impl.info.name, impl.info.driver, impl.info.api_major,
                impl.info.api_minor, impl.info.validation_enabled ? ", validation on" : "");

    // ── Allocator ───────────────────────────────────────────────────────────
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice   = impl.physical;
    allocator_info.device           = impl.device;
    allocator_info.instance         = impl.instance;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    allocator_info.pVulkanFunctions = &functions;
    if (vmaCreateAllocator(&allocator_info, &impl.allocator) != VK_SUCCESS) {
        return std::unexpected(RhiError::OutOfMemory);
    }

    // ── Pipeline cache, loaded from disk ────────────────────────────────────
    std::vector<u8> cache_data;
    if (!desc.pipeline_cache_path.empty()) {
        std::ifstream file(desc.pipeline_cache_path, std::ios::binary);
        if (file) {
            cache_data.assign(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
        }
    }
    VkPipelineCacheCreateInfo cache_info{};
    cache_info.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache_info.initialDataSize = cache_data.size();
    cache_info.pInitialData    = cache_data.empty() ? nullptr : cache_data.data();
    vkCreatePipelineCache(impl.device, &cache_info, nullptr, &impl.pipeline_cache);

    // ── Per-frame resources ─────────────────────────────────────────────────
    for (auto& frame : impl.frames) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = impl.queue_family;
        vkCreateCommandPool(impl.device, &pool_info, nullptr, &frame.command_pool);

        VkCommandBufferAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool        = frame.command_pool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers(impl.device, &allocate, &frame.command_buffer);

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(impl.device, &semaphore, nullptr, &frame.image_available);

        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // Signalled, so the first frame does not wait for a submit that never
        // happened.
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(impl.device, &fence, nullptr, &frame.in_flight);

        const std::array<VkDescriptorPoolSize, 1> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}};
        VkDescriptorPoolCreateInfo descriptor_pool{};
        descriptor_pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool.maxSets       = 64;
        descriptor_pool.poolSizeCount = static_cast<u32>(sizes.size());
        descriptor_pool.pPoolSizes    = sizes.data();
        vkCreateDescriptorPool(impl.device, &descriptor_pool, nullptr, &frame.descriptor_pool);
    }

    VkCommandPoolCreateInfo transfer_pool{};
    transfer_pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    transfer_pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    transfer_pool.queueFamilyIndex = impl.queue_family;
    vkCreateCommandPool(impl.device, &transfer_pool, nullptr, &impl.transfer_pool);

    // Two timestamps per frame in flight, written from the first frame. A
    // renderer that adds them later gets them after the decisions they should
    // have informed.
    if (properties.properties.limits.timestampComputeAndGraphics == VK_TRUE) {
        VkQueryPoolCreateInfo query{};
        query.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = kFramesInFlight * 2;
        vkCreateQueryPool(impl.device, &query, nullptr, &impl.timestamp_pool);
    }

    if (impl.surface != VK_NULL_HANDLE) {
        i32 width  = 0;
        i32 height = 0;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(desc.native_window), &width, &height);
        auto created = impl.create_swapchain(static_cast<u32>(std::max(width, 1)),
                                             static_cast<u32>(std::max(height, 1)));
        if (!created) {
            return std::unexpected(created.error());
        }
    }

    impl.command_list.device_ = &impl;
    return self;
}

void Device::Impl::save_pipeline_cache() const {
    if (desc.pipeline_cache_path.empty() || pipeline_cache == VK_NULL_HANDLE) {
        return;
    }
    usize size = 0;
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<u8> data(size);
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, data.data()) != VK_SUCCESS) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(desc.pipeline_cache_path).parent_path(), error);
    std::ofstream file(desc.pipeline_cache_path, std::ios::binary | std::ios::trunc);
    if (file) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
    }
}

const DeviceInfo& Device::info() const noexcept {
    return impl_->info;
}

f64 Device::last_frame_gpu_ms() const noexcept {
    return impl_->last_gpu_ms;
}

void Device::wait_idle() {
    vkDeviceWaitIdle(impl_->device);
}

}  // namespace ov::rhi
