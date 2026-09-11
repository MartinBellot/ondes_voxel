#define OV_LOG_CATEGORY "client"

#include "ov/client/sky_renderer.hpp"

#include "ov/base/log.hpp"
#include "ov/render/environment.hpp"

#include <array>
#include <span>

namespace ov::client {

namespace {

[[nodiscard]] constexpr std::array<f32, 4> unpack_rgb(u32 colour) noexcept {
    return {static_cast<f32>((colour >> 16) & 0xFFU) / 255.0F,
            static_cast<f32>((colour >> 8) & 0xFFU) / 255.0F,
            static_cast<f32>(colour & 0xFFU) / 255.0F, 1.0F};
}

}  // namespace

SkyRenderer::~SkyRenderer() {
    if (device_ == nullptr) {
        return;
    }
    if (pipeline_.valid()) {
        device_->destroy(pipeline_);
    }
    if (disc_.valid()) {
        device_->destroy(disc_);
    }
}

std::expected<std::unique_ptr<SkyRenderer>, rhi::RhiError> SkyRenderer::create(
    rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format) {
    std::unique_ptr<SkyRenderer> self(new SkyRenderer);
    self->device_ = &device;

    rhi::VertexBinding binding;
    binding.stride = 3 * sizeof(f32);
    binding.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rgb32Float, 0});

    rhi::GraphicsPipelineDesc pipeline;
    pipeline.vertex_shader             = "sky.vert.spv";
    pipeline.fragment_shader           = "sky.frag.spv";
    pipeline.vertex_bindings           = {binding};
    pipeline.layout.push_constant_size = sizeof(Push);
    pipeline.colour_format             = colour_format;
    pipeline.depth_format              = depth_format;
    // Tested against the cleared depth, which it always passes, and never
    // written: the terrain that follows is in front of the sky everywhere.
    pipeline.depth_test  = true;
    pipeline.depth_write = false;
    pipeline.cull_mode   = rhi::CullMode::None;
    pipeline.blend       = rhi::BlendMode::None;
    pipeline.topology    = rhi::PrimitiveTopology::TriangleList;
    pipeline.debug_name  = "sky disc";
    auto created = device.create_graphics_pipeline(pipeline);
    if (!created) {
        return std::unexpected(created.error());
    }
    self->pipeline_ = *created;

    const std::vector<f32> disc = render::sky_disc(render::kSkyDiscHeight);
    auto buffer = device.create_buffer(rhi::BufferDesc{disc.size() * sizeof(f32),
                                                       rhi::BufferUsage::Vertex, "sky disc", false});
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    self->disc_ = *buffer;
    if (auto uploaded = device.upload_buffer(
            self->disc_,
            std::span(reinterpret_cast<const u8*>(disc.data()), disc.size() * sizeof(f32)));
        !uploaded) {
        return std::unexpected(uploaded.error());
    }
    self->disc_vertices_ = static_cast<u32>(disc.size() / 3);
    return self;
}

void SkyRenderer::draw(rhi::CommandList& cmd, const SkyDraw& sky) {
    Push push;
    push.view_projection = sky.view_projection;
    push.colour          = unpack_rgb(sky.sky_colour);
    push.fog_colour      = unpack_rgb(sky.fog_colour);
    push.fog = {sky.fog_start, sky.fog_end, sky.spherical_fog ? 1.0F : 0.0F, 0.0F};

    cmd.bind_pipeline(pipeline_);
    cmd.push_constants(pipeline_, &push, sizeof(push));
    cmd.bind_vertex_buffer(0, disc_);
    cmd.draw(disc_vertices_);
}

}  // namespace ov::client
