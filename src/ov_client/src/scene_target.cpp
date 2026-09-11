#define OV_LOG_CATEGORY "client"

#include "ov/client/scene_target.hpp"

#include "ov/base/log.hpp"

#include <array>

namespace ov::client {

SceneTarget::~SceneTarget() {
    if (device_ == nullptr) {
        return;
    }
    if (pipeline_.valid()) {
        device_->destroy(pipeline_);
    }
    if (sampler_.valid()) {
        device_->destroy(sampler_);
    }
    if (image_.valid()) {
        device_->destroy(image_);
    }
}

std::expected<std::unique_ptr<SceneTarget>, rhi::RhiError> SceneTarget::create(
    rhi::Device& device, rhi::Format swapchain_format, u32 width, u32 height) {
    std::unique_ptr<SceneTarget> self(new SceneTarget);
    self->device_ = &device;

    auto sampler = device.create_sampler(rhi::SamplerDesc{rhi::Filter::Nearest, rhi::Filter::Nearest,
                                                          rhi::MipFilter::Nearest,
                                                          rhi::AddressMode::ClampToEdge, 1.0F, 0.0F});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    self->sampler_ = *sampler;

    rhi::GraphicsPipelineDesc pipeline;
    pipeline.vertex_shader               = "present.vert.spv";
    pipeline.fragment_shader             = "present.frag.spv";
    pipeline.layout.sampled_image_count  = 1;
    pipeline.colour_format               = swapchain_format;
    pipeline.depth_format                = rhi::Format::Undefined;
    pipeline.depth_test                  = false;
    pipeline.depth_write                 = false;
    pipeline.cull_mode                   = rhi::CullMode::None;
    pipeline.blend                       = rhi::BlendMode::None;
    pipeline.debug_name                  = "scene present";
    auto created = device.create_graphics_pipeline(pipeline);
    if (!created) {
        return std::unexpected(created.error());
    }
    self->pipeline_ = *created;

    if (auto sized = self->resize(width, height); !sized) {
        return std::unexpected(sized.error());
    }
    return self;
}

std::expected<void, rhi::RhiError> SceneTarget::resize(u32 width, u32 height) {
    if (image_.valid() && width == width_ && height == height_) {
        return {};
    }
    if (image_.valid()) {
        device_->destroy(image_);
        image_ = {};
    }
    auto image = device_->create_image(
        rhi::ImageDesc{width, height, 1, kFormat, true, true, "scene colour"});
    if (!image) {
        return std::unexpected(image.error());
    }
    image_  = *image;
    width_  = width;
    height_ = height;
    OV_LOG_INFO("scene target {}x{}, RGBA8 UNORM", width, height);
    return {};
}

void SceneTarget::present(rhi::CommandList& cmd) {
    const std::array<rhi::ImageHandle, 1>   images{image_};
    const std::array<rhi::SamplerHandle, 1> samplers{sampler_};
    cmd.bind_pipeline(pipeline_);
    cmd.bind_textures(pipeline_, images, samplers);
    cmd.draw(3);
}

}  // namespace ov::client
