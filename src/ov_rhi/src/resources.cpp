#define OV_LOG_CATEGORY "rhi"

#include "vulkan_common.hpp"

#include "ov/base/log.hpp"

#include <cstring>

namespace ov::rhi {

namespace {

[[nodiscard]] VkBufferUsageFlags usage_flags(BufferUsage usage) {
    // TRANSFER_DST on everything device-local, because the only way data gets
    // there is a staged copy.
    switch (usage) {
        // Both directions. An Upload buffer is the source of a staged upload and
        // the destination of a readback, and the validation layers catch the
        // missing half the first time a screenshot is taken.
        case BufferUsage::Upload:
            return VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Vertex:
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Index:
            return VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Uniform:
            return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Storage:
            return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        case BufferUsage::Indirect:
            return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return 0;
}

}  // namespace

std::expected<VkCommandBuffer, RhiError> Device::Impl::begin_one_shot() {
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool        = transfer_pool;
    allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &allocate, &cmd) != VK_SUCCESS) {
        return std::unexpected(RhiError::OutOfMemory);
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

std::expected<void, RhiError> Device::Impl::end_one_shot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo command{};
    command.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command.commandBuffer = cmd;

    VkSubmitInfo2 submit{};
    submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &command;

    // A one-shot upload is a start-up path. Blocking on a fence here is
    // simpler than a transfer queue and a pending list, and it costs nothing
    // that is measured in a frame.
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence    = VK_NULL_HANDLE;
    vkCreateFence(device, &fence_info, nullptr, &fence);

    const VkResult submitted = vkQueueSubmit2(queue, 1, &submit, fence);
    if (submitted == VK_SUCCESS) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transfer_pool, 1, &cmd);

    return submitted == VK_SUCCESS ? std::expected<void, RhiError>{}
                                   : std::unexpected(RhiError::DeviceLost);
}

std::expected<BufferHandle, RhiError> Device::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        return std::unexpected(RhiError::InvalidArgument);
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = desc.size;
    info.usage = usage_flags(desc.usage);

    VmaAllocationCreateInfo allocation{};
    if (desc.usage == BufferUsage::Upload || desc.host_visible) {
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    BufferResource    resource;
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(impl_->allocator, &info, &allocation, &resource.buffer,
                        &resource.allocation, &allocation_info) != VK_SUCCESS) {
        return std::unexpected(RhiError::OutOfMemory);
    }
    resource.mapped = allocation_info.pMappedData;
    resource.size   = desc.size;
    resource.usage  = desc.usage;

    return impl_->buffers.insert(resource);
}

std::expected<ImageHandle, RhiError> Device::create_image(const ImageDesc& desc) {
    const VkFormat format = to_vulkan(desc.format);
    const bool     depth  = is_depth_format(desc.format);

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = format;
    info.extent        = {desc.width, desc.height, 1};
    info.mipLevels     = desc.mip_levels;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (desc.sampled) {
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (desc.render_target) {
        info.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (info.usage == 0) {
        return std::unexpected(RhiError::InvalidArgument);
    }

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    ImageResource resource;
    if (vmaCreateImage(impl_->allocator, &info, &allocation, &resource.image, &resource.allocation,
                       nullptr) != VK_SUCCESS) {
        return std::unexpected(RhiError::OutOfMemory);
    }

    VkImageViewCreateInfo view{};
    view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image    = resource.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format   = format;
    view.subresourceRange.aspectMask =
        depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = desc.mip_levels;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(impl_->device, &view, nullptr, &resource.view) != VK_SUCCESS) {
        vmaDestroyImage(impl_->allocator, resource.image, resource.allocation);
        return std::unexpected(RhiError::OutOfMemory);
    }

    resource.format     = format;
    resource.width      = desc.width;
    resource.height     = desc.height;
    resource.mip_levels = desc.mip_levels;
    resource.depth      = depth;

    return impl_->images.insert(resource);
}

std::expected<SamplerHandle, RhiError> Device::create_sampler(const SamplerDesc& desc) {
    const auto filter = [](Filter value) {
        return value == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    };

    VkSamplerCreateInfo info{};
    info.sType      = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minFilter  = filter(desc.min_filter);
    info.magFilter  = filter(desc.mag_filter);
    info.mipmapMode = desc.mip_filter == MipFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                           : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const VkSamplerAddressMode address = desc.address_mode == AddressMode::Repeat
                                             ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                             : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeU                  = address;
    info.addressModeV                  = address;
    info.addressModeW                  = address;
    info.maxLod                        = desc.max_lod;
    if (desc.max_anisotropy > 1.0F) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy    = desc.max_anisotropy;
    }

    SamplerResource resource;
    if (vkCreateSampler(impl_->device, &info, nullptr, &resource.sampler) != VK_SUCCESS) {
        return std::unexpected(RhiError::OutOfMemory);
    }
    return impl_->samplers.insert(resource);
}

void Device::destroy(BufferHandle handle) {
    BufferResource resource;
    if (impl_->buffers.remove(handle, resource)) {
        vmaDestroyBuffer(impl_->allocator, resource.buffer, resource.allocation);
    }
}

void Device::destroy(ImageHandle handle) {
    ImageResource resource;
    if (impl_->images.remove(handle, resource)) {
        vkDestroyImageView(impl_->device, resource.view, nullptr);
        vmaDestroyImage(impl_->allocator, resource.image, resource.allocation);
    }
}

void Device::destroy(SamplerHandle handle) {
    SamplerResource resource;
    if (impl_->samplers.remove(handle, resource)) {
        vkDestroySampler(impl_->device, resource.sampler, nullptr);
    }
}

void Device::destroy(PipelineHandle handle) {
    PipelineResource resource;
    if (impl_->pipelines.remove(handle, resource)) {
        vkDestroyPipeline(impl_->device, resource.pipeline, nullptr);
        vkDestroyPipelineLayout(impl_->device, resource.layout, nullptr);
        if (resource.set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(impl_->device, resource.set_layout, nullptr);
        }
    }
}

void* Device::map(BufferHandle handle) {
    BufferResource* resource = impl_->buffers.get(handle);
    return resource != nullptr ? resource->mapped : nullptr;
}

bool Device::write_buffer(BufferHandle handle, const void* data, usize size, usize offset) {
    BufferResource* resource = impl_->buffers.get(handle);
    if (resource == nullptr || resource->mapped == nullptr || offset + size > resource->size) {
        return false;
    }
    std::memcpy(static_cast<u8*>(resource->mapped) + offset, data, size);
    return true;
}

std::expected<void, RhiError> Device::upload_buffer(BufferHandle handle, std::span<const u8> data,
                                                    u64 offset) {
    BufferResource* destination = impl_->buffers.get(handle);
    if (destination == nullptr || offset + data.size() > destination->size) {
        return std::unexpected(RhiError::InvalidArgument);
    }
    if (destination->mapped != nullptr) {
        std::memcpy(static_cast<u8*>(destination->mapped) + offset, data.data(), data.size());
        return {};
    }

    auto staging = create_buffer(BufferDesc{data.size(), BufferUsage::Upload, "staging"});
    if (!staging) {
        return std::unexpected(staging.error());
    }
    // Re-fetch: the pool is vector-backed, so creating the staging buffer may
    // have reallocated it and the pointer taken above is stale. Caught by the
    // validation layers as a copy into a VkBuffer that does not exist.
    destination = impl_->buffers.get(handle);
    std::memcpy(impl_->buffers.get(*staging)->mapped, data.data(), data.size());

    auto cmd = impl_->begin_one_shot();
    if (!cmd) {
        destroy(*staging);
        return std::unexpected(cmd.error());
    }

    VkBufferCopy region{};
    region.dstOffset = offset;
    region.size      = data.size();
    vkCmdCopyBuffer(*cmd, impl_->buffers.get(*staging)->buffer, destination->buffer, 1, &region);

    auto result = impl_->end_one_shot(*cmd);
    destroy(*staging);
    return result;
}

std::expected<void, RhiError> Device::upload_image(ImageHandle handle, std::span<const u8> pixels,
                                                   u32 width, u32 height, u32 mip_level) {
    ImageResource* image = impl_->images.get(handle);
    if (image == nullptr) {
        return std::unexpected(RhiError::InvalidArgument);
    }

    auto staging = create_buffer(BufferDesc{pixels.size(), BufferUsage::Upload, "image staging"});
    if (!staging) {
        return std::unexpected(staging.error());
    }
    // The image pool is a different vector, but the same rule applies to any
    // pointer held across a pool insertion.
    image = impl_->images.get(handle);
    std::memcpy(impl_->buffers.get(*staging)->mapped, pixels.data(), pixels.size());

    auto cmd = impl_->begin_one_shot();
    if (!cmd) {
        destroy(*staging);
        return std::unexpected(cmd.error());
    }

    // Undefined -> TransferDest -> ShaderRead, written out rather than tracked.
    // Only this mip level moves: uploading level 2 must not disturb the layout
    // of levels already in ShaderRead.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image            = image->image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip_level, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(*cmd, &dependency);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip_level, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyBufferToImage(*cmd, impl_->buffers.get(*staging)->buffer, image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(*cmd, &dependency);

    auto result = impl_->end_one_shot(*cmd);
    destroy(*staging);
    return result;
}

}  // namespace ov::rhi
