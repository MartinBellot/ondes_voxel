#define OV_LOG_CATEGORY "rhi"

#include "vulkan_common.hpp"

#include "ov/base/log.hpp"

#include <vector>

namespace ov::rhi {

namespace {

[[nodiscard]] VkCommandBuffer raw_of(u64 value) {
    return reinterpret_cast<VkCommandBuffer>(value);
}

}  // namespace

void CommandList::transition(ImageHandle image, ResourceState from, ResourceState to) {
    auto&                impl     = *static_cast<Device::Impl*>(device_);
    const ImageResource* resource = impl.images.get(image);
    if (resource == nullptr) {
        return;
    }

    const StateBarrier before = barrier_for(from);
    const StateBarrier after  = barrier_for(to);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask     = before.stage;
    barrier.srcAccessMask    = before.access;
    barrier.dstStageMask     = after.stage;
    barrier.dstAccessMask    = after.access;
    barrier.oldLayout        = before.layout;
    barrier.newLayout        = after.layout;
    barrier.image            = resource->image;
    barrier.subresourceRange = {
        resource->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0,
        resource->mip_levels, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(raw_of(raw_), &dependency);
}

void CommandList::transition_swapchain(ResourceState from, ResourceState to) {
    auto& impl = *static_cast<Device::Impl*>(device_);
    if (impl.swapchain_images.empty()) {
        return;
    }

    const StateBarrier before = barrier_for(from);
    const StateBarrier after  = barrier_for(to);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask     = before.stage;
    barrier.srcAccessMask    = before.access;
    barrier.dstStageMask     = after.stage;
    barrier.dstAccessMask    = after.access;
    barrier.oldLayout        = before.layout;
    barrier.newLayout        = after.layout;
    barrier.image            = impl.swapchain_images[impl.image_index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(raw_of(raw_), &dependency);
}

void CommandList::begin_rendering(std::span<const ColourAttachment> colour,
                                  const DepthAttachment* depth, u32 width, u32 height) {
    auto& impl = *static_cast<Device::Impl*>(device_);

    std::vector<VkRenderingAttachmentInfo> attachments;
    attachments.reserve(colour.size());
    for (const auto& target : colour) {
        VkRenderingAttachmentInfo info{};
        info.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        info.loadOp      = target.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        info.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        info.clearValue.color = {{target.clear_colour[0], target.clear_colour[1],
                                  target.clear_colour[2], target.clear_colour[3]}};

        if (target.image.valid()) {
            const ImageResource* resource = impl.images.get(target.image);
            info.imageView                = resource != nullptr ? resource->view : VK_NULL_HANDLE;
        } else {
            // An invalid handle means the swapchain image of this frame. It has
            // no ImageHandle because it is owned by the presentation engine and
            // its lifetime is not ours to hand out.
            info.imageView = impl.swapchain_views.empty() ? VK_NULL_HANDLE
                                                          : impl.swapchain_views[impl.image_index];
        }
        attachments.push_back(info);
    }

    VkRenderingAttachmentInfo depth_info{};
    if (depth != nullptr) {
        const ImageResource* resource = impl.images.get(depth->image);
        depth_info.sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_info.imageView          = resource != nullptr ? resource->view : VK_NULL_HANDLE;
        depth_info.imageLayout        = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_info.loadOp = depth->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_info.storeOp                 = VK_ATTACHMENT_STORE_OP_STORE;
        depth_info.clearValue.depthStencil = {depth->clear_depth, 0};
    }

    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, {width, height}};
    info.layerCount           = 1;
    info.colorAttachmentCount = static_cast<u32>(attachments.size());
    info.pColorAttachments    = attachments.data();
    info.pDepthAttachment     = depth != nullptr ? &depth_info : nullptr;

    vkCmdBeginRendering(raw_of(raw_), &info);
}

void CommandList::end_rendering() {
    vkCmdEndRendering(raw_of(raw_));
}

void CommandList::set_viewport(f32 x, f32 y, f32 width, f32 height) {
    // Negative height flips the viewport, so clip space matches the convention
    // the rest of the engine uses (+Y up) without a matrix hack in every
    // shader. Requires Vulkan 1.1, which every target has.
    VkViewport viewport{};
    viewport.x        = x;
    viewport.y        = y + height;
    viewport.width    = width;
    viewport.height   = -height;
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(raw_of(raw_), 0, 1, &viewport);
}

void CommandList::set_scissor(i32 x, i32 y, u32 width, u32 height) {
    VkRect2D scissor{{x, y}, {width, height}};
    vkCmdSetScissor(raw_of(raw_), 0, 1, &scissor);
}

void CommandList::bind_pipeline(PipelineHandle pipeline) {
    auto&                   impl     = *static_cast<Device::Impl*>(device_);
    const PipelineResource* resource = impl.pipelines.get(pipeline);
    if (resource == nullptr) {
        return;
    }
    vkCmdBindPipeline(raw_of(raw_), VK_PIPELINE_BIND_POINT_GRAPHICS, resource->pipeline);
}

void CommandList::bind_vertex_buffer(u32 binding, BufferHandle buffer, u64 offset) {
    auto&                 impl     = *static_cast<Device::Impl*>(device_);
    const BufferResource* resource = impl.buffers.get(buffer);
    if (resource == nullptr) {
        return;
    }
    vkCmdBindVertexBuffers(raw_of(raw_), binding, 1, &resource->buffer, &offset);
}

void CommandList::bind_index_buffer(BufferHandle buffer, u64 offset) {
    auto&                 impl     = *static_cast<Device::Impl*>(device_);
    const BufferResource* resource = impl.buffers.get(buffer);
    if (resource == nullptr) {
        return;
    }
    vkCmdBindIndexBuffer(raw_of(raw_), resource->buffer, offset, VK_INDEX_TYPE_UINT32);
}

void CommandList::bind_textures(PipelineHandle pipeline, std::span<const ImageHandle> images,
                                std::span<const SamplerHandle> samplers) {
    auto&                   impl     = *static_cast<Device::Impl*>(device_);
    const PipelineResource* resource = impl.pipelines.get(pipeline);
    if (resource == nullptr || resource->set_layout == VK_NULL_HANDLE || images.empty()) {
        return;
    }

    // One set per call, from a pool reset every frame. There is no cache to
    // invalidate and no lifetime to get wrong, and at a handful of draws a
    // frame the allocation is not worth optimising away yet.
    FrameResources& frame = impl.frames[impl.frame_index];

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool     = frame.descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts        = &resource->set_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(impl.device, &allocate, &set) != VK_SUCCESS) {
        OV_LOG_WARN("descriptor pool exhausted this frame");
        return;
    }

    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet>  writes;
    infos.reserve(images.size());
    writes.reserve(images.size());

    for (usize i = 0; i < images.size(); ++i) {
        const ImageResource*   image = impl.images.get(images[i]);
        const SamplerResource* sampler =
            i < samplers.size() ? impl.samplers.get(samplers[i]) : nullptr;
        if (image == nullptr || sampler == nullptr) {
            continue;
        }

        VkDescriptorImageInfo info{};
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.imageView   = image->view;
        info.sampler     = sampler->sampler;
        infos.push_back(info);

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = static_cast<u32>(i);
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.push_back(write);
    }
    // pImageInfo has to be filled after the vector has stopped growing, or the
    // pointers dangle.
    for (usize i = 0; i < writes.size(); ++i) {
        writes[i].pImageInfo = &infos[i];
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(impl.device, static_cast<u32>(writes.size()), writes.data(), 0,
                               nullptr);
    }
    vkCmdBindDescriptorSets(raw_of(raw_), VK_PIPELINE_BIND_POINT_GRAPHICS, resource->layout, 0, 1,
                            &set, 0, nullptr);
}

void CommandList::push_constants(PipelineHandle pipeline, const void* data, u32 size) {
    auto&                   impl     = *static_cast<Device::Impl*>(device_);
    const PipelineResource* resource = impl.pipelines.get(pipeline);
    if (resource == nullptr || size == 0) {
        return;
    }
    vkCmdPushConstants(raw_of(raw_), resource->layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, data);
}

void CommandList::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) {
    vkCmdDraw(raw_of(raw_), vertex_count, instance_count, first_vertex, first_instance);
}

void CommandList::draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                               i32 vertex_offset, u32 first_instance) {
    vkCmdDrawIndexed(raw_of(raw_), index_count, instance_count, first_index, vertex_offset,
                     first_instance);
}

void CommandList::draw_indexed_indirect(BufferHandle commands, u64 offset, u32 draw_count,
                                        u32 stride) {
    auto&                 impl     = *static_cast<Device::Impl*>(device_);
    const BufferResource* resource = impl.buffers.get(commands);
    if (resource == nullptr || draw_count == 0) {
        return;
    }
    vkCmdDrawIndexedIndirect(raw_of(raw_), resource->buffer, offset, draw_count, stride);
}

void CommandList::copy_buffer_to_image(BufferHandle source, u64 source_offset,
                                       ImageHandle destination, u32 mip_level, u32 width,
                                       u32 height) {
    auto&                 impl   = *static_cast<Device::Impl*>(device_);
    const BufferResource* buffer = impl.buffers.get(source);
    const ImageResource*  image  = impl.images.get(destination);
    if (buffer == nullptr || image == nullptr) {
        return;
    }

    VkBufferImageCopy region{};
    region.bufferOffset     = source_offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip_level, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyBufferToImage(raw_of(raw_), buffer->buffer, image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void CommandList::copy_swapchain_to_buffer(BufferHandle destination) {
    auto&                 impl   = *static_cast<Device::Impl*>(device_);
    const BufferResource* buffer = impl.buffers.get(destination);
    if (buffer == nullptr || impl.swapchain_images.empty()) {
        return;
    }

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {impl.swapchain_extent.width, impl.swapchain_extent.height, 1};
    vkCmdCopyImageToBuffer(raw_of(raw_), impl.swapchain_images[impl.image_index],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &region);
}

void CommandList::copy_buffer(BufferHandle source, u64 source_offset, BufferHandle destination,
                              u64 destination_offset, u64 size) {
    auto&                 impl = *static_cast<Device::Impl*>(device_);
    const BufferResource* from = impl.buffers.get(source);
    const BufferResource* to   = impl.buffers.get(destination);
    if (from == nullptr || to == nullptr || size == 0) {
        return;
    }

    VkBufferCopy region{};
    region.srcOffset = source_offset;
    region.dstOffset = destination_offset;
    region.size      = size;
    vkCmdCopyBuffer(raw_of(raw_), from->buffer, to->buffer, 1, &region);
}

}  // namespace ov::rhi
