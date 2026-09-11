#define OV_LOG_CATEGORY "client"

#include "ov/client/entity_renderer.hpp"

#include "ov/base/log.hpp"
#include "ov/render/mesher.hpp"

#include <array>
#include <cstring>

namespace ov::client {

namespace {

/// Must match entity.vert's push block exactly. The same 112 bytes the terrain
/// pushes, and the same meaning: sharing the layout is what keeps a mob's fog
/// and the ground's fog from drifting apart.
struct EntityPush {
    render::Mat4      view_projection;
    std::array<f32, 4> fog_colour{};
    std::array<f32, 4> camera_and_fog_start{};
    std::array<f32, 4> fog_end{};
};

static_assert(sizeof(EntityPush) == 112, "the push block must fit the guaranteed 128 bytes");

[[nodiscard]] f32 srgb_channel(u32 argb, u32 shift) noexcept {
    return static_cast<f32>((argb >> shift) & 0xFFU) / 255.0F;
}

}  // namespace

EntityRenderer::~EntityRenderer() {
    if (device_ == nullptr) {
        return;
    }
    for (const Texture& texture : textures_) {
        if (texture.owned) {
            device_->destroy(texture.image);
        }
    }
    for (const rhi::BufferHandle buffer : vertices_) {
        device_->destroy(buffer);
    }
    if (indices_.valid()) {
        device_->destroy(indices_);
    }
    if (sampler_.valid()) {
        device_->destroy(sampler_);
    }
    if (pipeline_.valid()) {
        device_->destroy(pipeline_);
    }
}

std::expected<std::unique_ptr<EntityRenderer>, rhi::RhiError> EntityRenderer::create(
    rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format, EntityPass pass) {
    std::unique_ptr<EntityRenderer> self(new EntityRenderer);
    self->device_ = &device;

    rhi::VertexBinding binding;
    binding.stride = sizeof(render::EntityVertex);
    binding.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rgb32Float, 0});
    binding.attributes.push_back(rhi::VertexAttribute{1, rhi::Format::Rg32Float, 12});
    binding.attributes.push_back(rhi::VertexAttribute{2, rhi::Format::Rgba8Unorm, 20});
    // ── entity-models ── the overlay texel and the lightmap sample.
    binding.attributes.push_back(rhi::VertexAttribute{3, rhi::Format::Rgba8Unorm, 24});
    binding.attributes.push_back(rhi::VertexAttribute{4, rhi::Format::Rgba8Unorm, 28});

    rhi::GraphicsPipelineDesc pipeline;
    pipeline.vertex_shader              = "entity.vert.spv";
    pipeline.fragment_shader            = "entity.frag.spv";
    pipeline.vertex_bindings            = {binding};
    pipeline.layout.sampled_image_count = 1;
    pipeline.layout.push_constant_size  = sizeof(EntityPush);
    pipeline.colour_format              = colour_format;
    pipeline.depth_format               = depth_format;
    pipeline.depth_test                 = true;
    pipeline.depth_write                = true;
    // ── entity-models ── Less-or-equal, as the game's entity render types: a
    // layer drawn over the same triangles (clothes over skin, eyes over a
    // face) sits at exactly the same depth.
    pipeline.depth_compare = rhi::CompareOp::LessOrEqual;
    // No culling: the game draws its mobs with `entity_cutout_no_cull`, and the
    // inside of a hat seen through its transparent texels is part of the
    // picture. The winding is still right (entity_mesh.cpp), it just is not
    // relied upon.
    pipeline.cull_mode = rhi::CullMode::None;
    // No blending. The overlay layers of a skin and the text on a sign are
    // cutouts, and the fragment shader discards below the same threshold the
    // terrain's cutout layer uses. Blending them instead would need a sort by
    // depth that a hundred moving mobs cannot be given cheaply.
    pipeline.blend      = rhi::BlendMode::None;
    pipeline.topology   = rhi::PrimitiveTopology::TriangleList;
    pipeline.debug_name = "entities";
    if (pass == EntityPass::Translucent) {  // ── weather ──
        pipeline.fragment_shader = "weather.frag.spv";
        pipeline.blend           = rhi::BlendMode::Alpha;
        pipeline.depth_write     = false;
        pipeline.depth_compare   = rhi::CompareOp::Less;
        pipeline.cull_mode       = rhi::CullMode::None;
        pipeline.debug_name      = "weather";
    }
    // ── entity-models ──
    if (pass == EntityPass::EntityTranslucent) {
        pipeline.fragment_shader = "entity_translucent.frag.spv";
        pipeline.blend           = rhi::BlendMode::Alpha;
        pipeline.debug_name      = "entities translucent";
    }
    if (pass == EntityPass::Eyes || pass == EntityPass::Energy) {
        pipeline.fragment_shader = "entity_additive.frag.spv";
        pipeline.blend           = rhi::BlendMode::Additive;
        pipeline.depth_write     = false;
        pipeline.debug_name      = pass == EntityPass::Eyes ? "entity eyes" : "entity energy";
    }
    if (pass == EntityPass::Crumbling) {  // ── breaking ──
        pipeline.fragment_shader     = "crumbling.frag.spv";
        pipeline.blend               = rhi::BlendMode::Multiply;
        pipeline.depth_write         = false;
        pipeline.depth_compare       = rhi::CompareOp::LessOrEqual;
        pipeline.depth_bias_constant = -10.0F;
        pipeline.depth_bias_slope    = -1.0F;
        // The cracks keep the back-face culling they were measured with: the
        // entity passes above dropped culling for the game's no-cull mobs, and
        // a block's cracks are not one of them.
        pipeline.cull_mode  = rhi::CullMode::Back;
        pipeline.debug_name = "crumbling";
    }

    auto created = device.create_graphics_pipeline(pipeline);
    if (!created) {
        return std::unexpected(created.error());
    }
    self->pipeline_ = *created;

    // Nearest, and no mips. An entity texture is addressed at texel granularity
    // with no padding between one cube's net and the next; a mip would bleed
    // the neighbour's colour across the seam of every limb.
    auto sampler = device.create_sampler(rhi::SamplerDesc{rhi::Filter::Nearest,
                                                          rhi::Filter::Nearest,
                                                          rhi::MipFilter::Nearest,
                                                          pass == EntityPass::Translucent ||
                                                                  pass == EntityPass::Energy ||
                                                                  pass == EntityPass::Crumbling
                                                              ? rhi::AddressMode::Repeat
                                                              : rhi::AddressMode::ClampToEdge,
                                                          1.0F,
                                                          0.0F});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    self->sampler_ = *sampler;

    const std::vector<u32> quad_indices = render::build_shared_quad_indices(kMaxQuads);
    auto                   indices      = device.create_buffer(rhi::BufferDesc{
        quad_indices.size() * sizeof(u32), rhi::BufferUsage::Index, "entity indices", false});
    if (!indices) {
        return std::unexpected(indices.error());
    }
    self->indices_ = *indices;
    if (auto uploaded = device.upload_buffer(
            *indices, std::span<const u8>(reinterpret_cast<const u8*>(quad_indices.data()),
                                          quad_indices.size() * sizeof(u32)));
        !uploaded) {
        return std::unexpected(uploaded.error());
    }

    const u32 ring = rhi::Device::frames_in_flight();
    for (u32 slot = 0; slot < ring; ++slot) {
        auto buffer = device.create_buffer(
            rhi::BufferDesc{static_cast<usize>(kMaxQuads) * 4 * sizeof(render::EntityVertex),
                            rhi::BufferUsage::Vertex, "entity vertices", true});
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        self->vertices_.push_back(*buffer);
    }

    self->scratch_.reserve(static_cast<usize>(kMaxQuads) * 4);
    self->batches_.reserve(32);
    return self;
}

std::expected<EntityTexture, rhi::RhiError> EntityRenderer::add_texture(
    const render::TextureImage& image, std::string_view name) {
    if (image.empty()) {
        return std::unexpected(rhi::RhiError::InvalidArgument);
    }
    rhi::ImageDesc desc;
    desc.width      = image.width;
    desc.height     = image.height;
    desc.mip_levels = 1;
    desc.format     = rhi::Format::Rgba8Srgb;
    desc.sampled    = true;
    desc.debug_name = name;

    auto created = device_->create_image(desc);
    if (!created) {
        return std::unexpected(created.error());
    }
    if (auto uploaded = device_->upload_image(*created, image.rgba, image.width, image.height);
        !uploaded) {
        device_->destroy(*created);
        return std::unexpected(uploaded.error());
    }
    textures_.push_back(Texture{*created, image.width, image.height, true, {}});
    return static_cast<EntityTexture>(textures_.size() - 1);
}

EntityTexture EntityRenderer::borrow_texture(rhi::ImageHandle image, u32 width, u32 height) {
    textures_.push_back(Texture{image, width, height, false, {}});
    return static_cast<EntityTexture>(textures_.size() - 1);
}

rhi::ImageHandle EntityRenderer::image(EntityTexture texture) const noexcept {  // ── entity-models ──
    const auto index = static_cast<usize>(texture);
    return index < textures_.size() ? textures_[index].image : rhi::ImageHandle{};
}

void EntityRenderer::begin() {
    for (Texture& texture : textures_) {
        texture.pending.clear();
    }
    scratch_.clear();
    batches_.clear();
    pending_quads_  = 0;
    stats_.entities = 0;
    stats_.quads    = 0;
    stats_.draws    = 0;
    stats_.vertices = 0;
    stats_.dropped  = 0;
}

bool EntityRenderer::submit(EntityTexture                         texture,
                            std::span<const render::EntityVertex> vertices) {
    if (vertices.empty()) {
        return true;
    }
    const auto index = static_cast<usize>(texture);
    if (index >= textures_.size()) {
        ++stats_.dropped;
        return false;
    }
    const auto quads = static_cast<u32>(vertices.size() / 4);
    if (pending_quads_ + quads > kMaxQuads) {
        ++stats_.dropped;
        return false;
    }

    // Filed by texture, not appended in arrival order. See the header: batching
    // by arrival cut a batch per mob and ran the descriptor pool dry at
    // sixty-four.
    Texture& entry = textures_[index];
    entry.pending.insert(entry.pending.end(), vertices.begin(), vertices.end());
    pending_quads_ += quads;
    ++stats_.entities;
    stats_.quads += quads;
    return true;
}

void EntityRenderer::draw(rhi::CommandList& cmd, const render::Mat4& view_projection,
                          Vec3f camera, const EntitySky& sky) {
    scratch_.clear();
    batches_.clear();
    for (Texture& texture : textures_) {
        if (texture.pending.empty()) {
            continue;
        }
        batches_.push_back(Batch{texture.image, static_cast<u32>(scratch_.size() / 4),
                                 static_cast<u32>(texture.pending.size() / 4)});
        scratch_.insert(scratch_.end(), texture.pending.begin(), texture.pending.end());
    }

    stats_.vertices      = static_cast<u32>(scratch_.size());
    stats_.peak_vertices = std::max(stats_.peak_vertices, stats_.vertices);
    if (scratch_.empty()) {
        return;
    }

    const u32 slot   = ring_ % static_cast<u32>(vertices_.size());
    auto*     mapped = device_->map(vertices_[slot]);
    if (mapped == nullptr) {
        OV_LOG_WARN("the entity vertex ring is not host visible; nothing drawn");
        return;
    }
    std::memcpy(mapped, scratch_.data(), scratch_.size() * sizeof(render::EntityVertex));

    EntityPush push;
    push.view_projection = view_projection;
    push.fog_colour      = {srgb_channel(sky.fog_colour, 16), srgb_channel(sky.fog_colour, 8),
                            srgb_channel(sky.fog_colour, 0), 1.0F};
    push.camera_and_fog_start = {camera.x, camera.y, camera.z, sky.fog_start};
    push.fog_end              = {sky.fog_end, 0.0F, 0.0F, 0.0F};

    cmd.bind_pipeline(pipeline_);
    cmd.push_constants(pipeline_, &push, sizeof(push));
    cmd.bind_vertex_buffer(0, vertices_[slot]);
    cmd.bind_index_buffer(indices_);

    for (const Batch& batch : batches_) {
        cmd.bind_textures(pipeline_, std::array<rhi::ImageHandle, 1>{batch.image},
                          std::array<rhi::SamplerHandle, 1>{sampler_});
        // Six indices a quad, four vertices a quad: the shared index buffer is
        // written for quad 0 at vertex 0, so the first index follows from the
        // batch's first quad.
        cmd.draw_indexed(batch.count * 6, 1, batch.first * 6, 0, 0);
        ++stats_.draws;
    }

    ring_ = (ring_ + 1) % static_cast<u32>(vertices_.size());
}

}  // namespace ov::client
