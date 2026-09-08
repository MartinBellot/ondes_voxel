// The one place Vulkan exists. Private to ov_rhi; never reachable from another
// module, because ov_add_library puts src/ on the private include path only.
#pragma once

#include "ov/rhi/device.hpp"
#include "ov/rhi/handle.hpp"
#include "ov/rhi/types.hpp"

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <string>
#include <vector>

namespace ov::rhi {

/// Two frames in flight. Not three: a third costs a frame of latency and a
/// third copy of every per-frame buffer, and on a unified-memory part the
/// bandwidth that buys it back is the scarce resource, not the GPU.
inline constexpr u32 kFramesInFlight = 2;

struct BufferResource {
    VkBuffer      buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    void*         mapped{nullptr};
    VkDeviceSize  size{0};
    BufferUsage   usage{BufferUsage::Upload};
};

struct ImageResource {
    VkImage       image{VK_NULL_HANDLE};
    VkImageView   view{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    VkFormat      format{VK_FORMAT_UNDEFINED};
    u32           width{0};
    u32           height{0};
    u32           mip_levels{1};
    bool          depth{false};
};

struct SamplerResource {
    VkSampler sampler{VK_NULL_HANDLE};
};

struct PipelineResource {
    VkPipeline            pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout{VK_NULL_HANDLE};
    u32                   sampled_image_count{0};
    u32                   storage_buffer_count{0};
    u32                   push_constant_size{0};
};

/// Everything owned per frame in flight.
struct FrameResources {
    VkCommandPool   command_pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    VkSemaphore     image_available{VK_NULL_HANDLE};
    VkFence         in_flight{VK_NULL_HANDLE};
    /// Reset every frame; descriptor sets live exactly one frame, which is why
    /// there is no cache to invalidate and no lifetime to get wrong.
    VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
};

struct Device::Impl {
    VkInstance               instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    VkPhysicalDevice         physical{VK_NULL_HANDLE};
    VkDevice                 device{VK_NULL_HANDLE};
    VkQueue                  queue{VK_NULL_HANDLE};
    u32                      queue_family{0};
    VmaAllocator             allocator{nullptr};
    VkPipelineCache          pipeline_cache{VK_NULL_HANDLE};

    VkSurfaceKHR   surface{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat       swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D     swapchain_extent{0, 0};

    std::vector<VkImage>     swapchain_images;
    std::vector<VkImageView> swapchain_views;
    /// One per swapchain image, not per frame in flight: a semaphore signalled
    /// by a present cannot be reused until that image comes back, and tying it
    /// to the frame index instead is the classic validation error here.
    std::vector<VkSemaphore> render_finished;

    std::array<FrameResources, kFramesInFlight> frames{};
    u32                                         frame_index{0};
    u32                                         image_index{0};
    bool                                        frame_open{false};

    VkQueryPool timestamp_pool{VK_NULL_HANDLE};
    f64         timestamp_period{0.0};
    f64         last_gpu_ms{0.0};
    /// Frames whose timestamps have actually been written.
    std::array<bool, kFramesInFlight> timestamps_valid{};

    /// A command pool for the synchronous start-up uploads.
    VkCommandPool transfer_pool{VK_NULL_HANDLE};

    HandlePool<BufferResource, BufferTag>     buffers;
    HandlePool<ImageResource, ImageTag>       images;
    HandlePool<SamplerResource, SamplerTag>   samplers;
    HandlePool<PipelineResource, PipelineTag> pipelines;

    CommandList command_list;
    DeviceInfo  info;
    DeviceDesc  desc;

    [[nodiscard]] std::expected<VkCommandBuffer, RhiError> begin_one_shot();
    [[nodiscard]] std::expected<void, RhiError>            end_one_shot(VkCommandBuffer cmd);

    [[nodiscard]] std::expected<void, RhiError> create_swapchain(u32 width, u32 height);
    void                                        destroy_swapchain();
    void                                        save_pipeline_cache() const;
};

[[nodiscard]] VkFormat to_vulkan(Format format) noexcept;

[[nodiscard]] Format from_vulkan(VkFormat format) noexcept;

/// The barrier a transition means: layout, access mask and stage, in one place
/// so that the six states cannot drift apart between call sites.
struct StateBarrier {
    VkImageLayout         layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkAccessFlags2        access{0};
    VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
};

[[nodiscard]] StateBarrier barrier_for(ResourceState state) noexcept;

}  // namespace ov::rhi
