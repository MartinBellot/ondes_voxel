#define OV_LOG_CATEGORY "client"

#include "ov/client/sky_renderer.hpp"

#include "ov/base/log.hpp"
#include "ov/render/environment.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace ov::client {

namespace {

[[nodiscard]] constexpr std::array<f32, 4> unpack_rgb(u32 colour) noexcept {
    return {static_cast<f32>((colour >> 16) & 0xFFU) / 255.0F,
            static_cast<f32>((colour >> 8) & 0xFFU) / 255.0F,
            static_cast<f32>(colour & 0xFFU) / 255.0F, 1.0F};
}

/// The band's sixteen triangles at seven floats a vertex, and two quads of
/// six vertices at five floats: what one frame's dynamic buffer holds.
constexpr usize kBandFloats   = 16 * 3 * 7;
constexpr usize kBodyFloats   = 2 * 6 * 5;
constexpr usize kDynamicBytes = (kBandFloats + kBodyFloats) * sizeof(f32);

[[nodiscard]] std::expected<rhi::ImageHandle, rhi::RhiError> upload(
    rhi::Device& device, const render::TextureImage& image, std::string_view name) {
    rhi::ImageDesc desc;
    desc.width      = image.width;
    desc.height     = image.height;
    desc.mip_levels = 1;
    desc.format     = rhi::Format::Rgba8Srgb;
    desc.sampled    = true;
    desc.debug_name = name;
    auto created = device.create_image(desc);
    if (!created) {
        return std::unexpected(created.error());
    }
    if (auto uploaded = device.upload_image(*created, image.rgba, image.width, image.height);
        !uploaded) {
        device.destroy(*created);
        return std::unexpected(uploaded.error());
    }
    return *created;
}

}  // namespace

SkyRenderer::~SkyRenderer() {
    if (device_ == nullptr) {
        return;
    }
    for (const auto pipeline : {pipeline_, band_pipeline_, body_pipeline_}) {
        if (pipeline.valid()) {
            device_->destroy(pipeline);
        }
    }
    for (const auto buffer : dynamic_) {
        device_->destroy(buffer);
    }
    if (disc_.valid()) {
        device_->destroy(disc_);
    }
    if (sampler_.valid()) {
        device_->destroy(sampler_);
    }
    for (const auto image : {sun_, moon_}) {
        if (image.valid()) {
            device_->destroy(image);
        }
    }
}

std::expected<std::unique_ptr<SkyRenderer>, rhi::RhiError> SkyRenderer::create(
    rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format,
    const render::TextureImage* sun, const render::TextureImage* moon) {
    std::unique_ptr<SkyRenderer> self(new SkyRenderer);
    self->device_ = &device;

    const auto pipeline_desc = [&](std::string_view vertex, std::string_view fragment,
                                   rhi::VertexBinding binding, u32 push_size, u32 images,
                                   rhi::BlendMode blend, std::string_view name) {
        rhi::GraphicsPipelineDesc pipeline;
        pipeline.vertex_shader               = vertex;
        pipeline.fragment_shader             = fragment;
        pipeline.vertex_bindings             = {std::move(binding)};
        pipeline.layout.push_constant_size   = push_size;
        pipeline.layout.sampled_image_count  = images;
        pipeline.colour_format               = colour_format;
        pipeline.depth_format                = depth_format;
        // Tested against the cleared depth, which it always passes, and never
        // written: the terrain that follows is in front of the sky everywhere.
        pipeline.depth_test  = true;
        pipeline.depth_write = false;
        pipeline.cull_mode   = rhi::CullMode::None;
        pipeline.blend       = blend;
        pipeline.topology    = rhi::PrimitiveTopology::TriangleList;
        pipeline.debug_name  = name;
        return device.create_graphics_pipeline(pipeline);
    };

    rhi::VertexBinding position;
    position.stride = 3 * sizeof(f32);
    position.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rgb32Float, 0});
    auto disc = pipeline_desc("sky.vert.spv", "sky.frag.spv", position, sizeof(Push), 0,
                              rhi::BlendMode::None, "sky disc");
    if (!disc) {
        return std::unexpected(disc.error());
    }
    self->pipeline_ = *disc;

    rhi::VertexBinding coloured;
    coloured.stride = 7 * sizeof(f32);
    coloured.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rgb32Float, 0});
    coloured.attributes.push_back(rhi::VertexAttribute{1, rhi::Format::Rgba32Float, 12});
    auto band = pipeline_desc("sky_colour.vert.spv", "sky_colour.frag.spv", coloured,
                              sizeof(SimplePush), 0, rhi::BlendMode::Alpha, "sky twilight band");
    if (!band) {
        return std::unexpected(band.error());
    }
    self->band_pipeline_ = *band;

    rhi::VertexBinding textured;
    textured.stride = 5 * sizeof(f32);
    textured.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rgb32Float, 0});
    textured.attributes.push_back(rhi::VertexAttribute{1, rhi::Format::Rg32Float, 12});
    auto body = pipeline_desc("sky_texture.vert.spv", "sky_texture.frag.spv", textured,
                              sizeof(SimplePush), 1, rhi::BlendMode::Additive, "sky sun and moon");
    if (!body) {
        return std::unexpected(body.error());
    }
    self->body_pipeline_ = *body;

    const std::vector<f32> geometry = render::sky_disc(render::kSkyDiscHeight);
    auto buffer = device.create_buffer(rhi::BufferDesc{geometry.size() * sizeof(f32),
                                                       rhi::BufferUsage::Vertex, "sky disc", false});
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    self->disc_ = *buffer;
    if (auto uploaded = device.upload_buffer(
            self->disc_, std::span(reinterpret_cast<const u8*>(geometry.data()),
                                   geometry.size() * sizeof(f32)));
        !uploaded) {
        return std::unexpected(uploaded.error());
    }
    self->disc_vertices_ = static_cast<u32>(geometry.size() / 3);

    for (u32 i = 0; i < rhi::Device::frames_in_flight(); ++i) {
        auto dynamic = device.create_buffer(
            rhi::BufferDesc{kDynamicBytes, rhi::BufferUsage::Vertex, "sky band and bodies", true});
        if (!dynamic) {
            return std::unexpected(dynamic.error());
        }
        self->dynamic_.push_back(*dynamic);
    }

    // Pixel art: the sun's square and the moon's phases stay square.
    auto sampler = device.create_sampler(rhi::SamplerDesc{rhi::Filter::Nearest, rhi::Filter::Nearest,
                                                          rhi::MipFilter::Nearest,
                                                          rhi::AddressMode::ClampToEdge, 1.0F, 0.0F});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    self->sampler_ = *sampler;
    if (sun != nullptr && !sun->empty()) {
        if (auto image = upload(device, *sun, "sun"); image) {
            self->sun_ = *image;
        }
    }
    if (moon != nullptr && !moon->empty()) {
        if (auto image = upload(device, *moon, "moon phases"); image) {
            self->moon_ = *image;
        }
    }
    if (!self->sun_.valid() || !self->moon_.valid()) {
        OV_LOG_WARN("sky: no {} texture — drawn without it",
                    !self->sun_.valid() ? "sun" : "moon");
    }
    self->scratch_.reserve(kBandFloats + kBodyFloats);
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

    // The band, then the sun and the moon: the order the game draws them in,
    // and the order their blending needs.
    const rhi::BufferHandle buffer = dynamic_[device_->frame_index() % dynamic_.size()];
    auto*                   mapped = static_cast<u8*>(device_->map(buffer));
    if (mapped == nullptr) {
        return;
    }
    scratch_.clear();
    u32 band_vertices = 0;
    if (sky.sunrise) {
        const std::vector<f32> fan = render::sunrise_fan(*sky.sunrise, sky.celestial);
        scratch_.insert(scratch_.end(), fan.begin(), fan.end());
        band_vertices = static_cast<u32>(fan.size() / 7);
    }
    const usize body_start = scratch_.size() * sizeof(f32);
    const auto  quad       = [this](const std::array<Vec3f, 4>& corner,
                               const std::array<std::array<f32, 2>, 4>& uv) {
        for (const usize i : {0U, 1U, 2U, 0U, 2U, 3U}) {
            scratch_.insert(scratch_.end(),
                            {corner[i].x, corner[i].y, corner[i].z, uv[i][0], uv[i][1]});
        }
    };
    quad(render::sun_quad(sky.celestial), {{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}});
    // moon_phases.png is four phases across and two down.
    const i32 phase = ((sky.moon_phase % 8) + 8) % 8;
    const f32 u0    = static_cast<f32>(phase % 4) / 4.0F;
    const f32 v0    = static_cast<f32>(phase / 4) / 2.0F;
    const f32 u1    = u0 + 0.25F;
    const f32 v1    = v0 + 0.5F;
    quad(render::moon_quad(sky.celestial), {{{u1, v1}, {u0, v1}, {u0, v0}, {u1, v0}}});
    std::memcpy(mapped, scratch_.data(), scratch_.size() * sizeof(f32));

    SimplePush simple;
    simple.view_projection = sky.view_projection;
    if (band_vertices > 0) {
        simple.colour = {1.0F, 1.0F, 1.0F, 1.0F};
        cmd.bind_pipeline(band_pipeline_);
        cmd.push_constants(band_pipeline_, &simple, sizeof(simple));
        cmd.bind_vertex_buffer(0, buffer);
        cmd.draw(band_vertices);
    }

    const f32 clear = 1.0F - std::clamp(sky.rain, 0.0F, 1.0F);
    simple.colour   = {1.0F, 1.0F, 1.0F, clear};
    const u32 first = static_cast<u32>(body_start / (5 * sizeof(f32)));
    const auto body = [&](rhi::ImageHandle image, u32 offset) {
        if (!image.valid()) {
            return;
        }
        const std::array<rhi::ImageHandle, 1>   images{image};
        const std::array<rhi::SamplerHandle, 1> samplers{sampler_};
        cmd.bind_pipeline(body_pipeline_);
        cmd.bind_textures(body_pipeline_, images, samplers);
        cmd.push_constants(body_pipeline_, &simple, sizeof(simple));
        cmd.bind_vertex_buffer(0, buffer);
        cmd.draw(6, 1, first + offset, 0);
    };
    body(sun_, 0);
    body(moon_, 6);
}

}  // namespace ov::client
