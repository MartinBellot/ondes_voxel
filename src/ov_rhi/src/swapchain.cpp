#define OV_LOG_CATEGORY "rhi"

#include "vulkan_common.hpp"

#include "ov/base/log.hpp"

#include <algorithm>

namespace ov::rhi {

std::expected<void, RhiError> Device::Impl::create_swapchain(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities) != VK_SUCCESS) {
        return std::unexpected(RhiError::NoSurface);
    }

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
    if (formats.empty()) {
        return std::unexpected(RhiError::NoSurface);
    }

    // An sRGB swapchain, so the hardware does the encode. Minecraft's textures
    // are authored in sRGB and blending them as if they were linear is the
    // single most common reason a reimplementation looks washed out.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB &&
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = candidate;
            break;
        }
    }

    VkExtent2D extent{width, height};
    if (capabilities.currentExtent.width != 0xFFFFFFFFu) {
        extent = capabilities.currentExtent;
    } else {
        extent.width  = std::clamp(extent.width, capabilities.minImageExtent.width,
                                   capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        // A minimised window. Not an error; the caller skips the frame.
        return std::unexpected(RhiError::SwapchainOutOfDate);
    }

    u32 image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR create{};
    create.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create.surface          = surface;
    create.minImageCount    = image_count;
    create.imageFormat      = chosen.format;
    create.imageColorSpace  = chosen.colorSpace;
    create.imageExtent      = extent;
    create.imageArrayLayers = 1;
    // TRANSFER_SRC as well as COLOR_ATTACHMENT, so a frame can be read back for
    // a golden image without a second render target.
    create.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform     = capabilities.currentTransform;
    create.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is the only mode every implementation must support, and it is the
    // right default: tearing is not a trade this project wants to make by
    // accident.
    create.presentMode  = VK_PRESENT_MODE_FIFO_KHR;
    create.clipped      = VK_TRUE;
    create.oldSwapchain = swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device, &create, nullptr, &created) != VK_SUCCESS) {
        return std::unexpected(RhiError::NoSurface);
    }

    destroy_swapchain();
    swapchain        = created;
    swapchain_format = chosen.format;
    swapchain_extent = extent;

    u32 count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    swapchain_images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data());

    swapchain_views.resize(count);
    render_finished.resize(count);
    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                       = swapchain_images[i];
        view_info.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                      = swapchain_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &view_info, nullptr, &swapchain_views[i]);

        // One per image rather than per frame in flight. A semaphore signalled
        // by a submit that a present is waiting on cannot be reused until that
        // image comes back around, and indexing it by frame is the mistake the
        // validation layers report as a semaphore already in use.
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device, &semaphore, nullptr, &render_finished[i]);
    }

    OV_LOG_INFO("swapchain {}x{}, {} images", extent.width, extent.height, count);
    return {};
}

void Device::Impl::destroy_swapchain() {
    for (VkImageView view : swapchain_views) {
        vkDestroyImageView(device, view, nullptr);
    }
    for (VkSemaphore semaphore : render_finished) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    swapchain_views.clear();
    render_finished.clear();
    swapchain_images.clear();
    // The swapchain itself is retired by the next vkCreateSwapchainKHR through
    // oldSwapchain, and destroyed here only on teardown.
}

std::expected<void, RhiError> Device::resize(u32 width, u32 height) {
    if (impl_->surface == VK_NULL_HANDLE) {
        return {};
    }
    vkDeviceWaitIdle(impl_->device);

    VkSwapchainKHR old     = impl_->swapchain;
    auto           created = impl_->create_swapchain(width, height);
    if (old != VK_NULL_HANDLE && old != impl_->swapchain) {
        vkDestroySwapchainKHR(impl_->device, old, nullptr);
    }
    return created;
}

u32 Device::swapchain_width() const noexcept {
    return impl_->swapchain_extent.width;
}

u32 Device::swapchain_height() const noexcept {
    return impl_->swapchain_extent.height;
}

Format Device::swapchain_format() const noexcept {
    return from_vulkan(impl_->swapchain_format);
}

// ── Frames ──────────────────────────────────────────────────────────────────

std::expected<CommandList*, RhiError> Device::begin_frame() {
    Impl&           impl  = *impl_;
    FrameResources& frame = impl.frames[impl.frame_index];

    vkWaitForFences(impl.device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

    // Read the previous submission's timestamps before the pool slot is reused.
    if (impl.timestamp_pool != VK_NULL_HANDLE && impl.timestamps_valid[impl.frame_index]) {
        std::array<u64, 2> stamps{};
        if (vkGetQueryPoolResults(impl.device, impl.timestamp_pool, impl.frame_index * 2, 2,
                                  sizeof(stamps), stamps.data(), sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            impl.last_gpu_ms =
                static_cast<f64>(stamps[1] - stamps[0]) * impl.timestamp_period / 1.0e6;
        }
    }

    if (impl.swapchain != VK_NULL_HANDLE) {
        const VkResult acquired =
            vkAcquireNextImageKHR(impl.device, impl.swapchain, UINT64_MAX, frame.image_available,
                                  VK_NULL_HANDLE, &impl.image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            return std::unexpected(RhiError::SwapchainOutOfDate);
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            return std::unexpected(RhiError::DeviceLost);
        }
    }

    // Only now, past every early return: resetting the fence before a return
    // path leaves it unsignalled with nothing to signal it, and the next frame
    // waits for ever.
    vkResetFences(impl.device, 1, &frame.in_flight);
    vkResetCommandPool(impl.device, frame.command_pool, 0);
    vkResetDescriptorPool(impl.device, frame.descriptor_pool, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.command_buffer, &begin);

    if (impl.timestamp_pool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.command_buffer, impl.timestamp_pool, impl.frame_index * 2, 2);
        vkCmdWriteTimestamp2(frame.command_buffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             impl.timestamp_pool, impl.frame_index * 2);
    }

    impl.frame_open           = true;
    impl.command_list.raw_    = reinterpret_cast<u64>(frame.command_buffer);
    impl.command_list.device_ = &impl;
    return &impl.command_list;
}

std::expected<void, RhiError> Device::end_frame() {
    Impl&           impl  = *impl_;
    FrameResources& frame = impl.frames[impl.frame_index];
    if (!impl.frame_open) {
        return std::unexpected(RhiError::InvalidArgument);
    }

    if (impl.timestamp_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(frame.command_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             impl.timestamp_pool, impl.frame_index * 2 + 1);
        impl.timestamps_valid[impl.frame_index] = true;
    }

    vkEndCommandBuffer(frame.command_buffer);

    VkCommandBufferSubmitInfo command{};
    command.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command.commandBuffer = frame.command_buffer;

    VkSemaphoreSubmitInfo wait{};
    wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = frame.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal{};
    signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (impl.swapchain != VK_NULL_HANDLE) {
        signal.semaphore = impl.render_finished[impl.image_index];
    }

    VkSubmitInfo2 submit{};
    submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &command;
    submit.waitSemaphoreInfoCount   = impl.swapchain != VK_NULL_HANDLE ? 1u : 0u;
    submit.pWaitSemaphoreInfos      = &wait;
    submit.signalSemaphoreInfoCount = impl.swapchain != VK_NULL_HANDLE ? 1u : 0u;
    submit.pSignalSemaphoreInfos    = &signal;

    if (vkQueueSubmit2(impl.queue, 1, &submit, frame.in_flight) != VK_SUCCESS) {
        return std::unexpected(RhiError::DeviceLost);
    }

    std::expected<void, RhiError> result;
    if (impl.swapchain != VK_NULL_HANDLE) {
        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &impl.render_finished[impl.image_index];
        present.swapchainCount     = 1;
        present.pSwapchains        = &impl.swapchain;
        present.pImageIndices      = &impl.image_index;

        const VkResult presented = vkQueuePresentKHR(impl.queue, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            result = std::unexpected(RhiError::SwapchainOutOfDate);
        }
    }

    impl.frame_open  = false;
    impl.frame_index = (impl.frame_index + 1) % kFramesInFlight;
    return result;
}

}  // namespace ov::rhi
