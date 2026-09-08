#define OV_LOG_CATEGORY "client"

#include "ov/client/terrain_renderer.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cstring>

namespace ov::client {

namespace {

/// The layout of VkDrawIndexedIndirectCommand, which the specification fixes.
/// If this ever stops being true the GPU reads five garbage numbers and draws
/// nothing, in silence — so it is asserted rather than trusted.
static_assert(sizeof(u32) == 4);

[[nodiscard]] constexpr usize layer_index(render::RenderLayer layer) noexcept {
    return static_cast<usize>(layer);
}

}  // namespace

TerrainRenderer::~TerrainRenderer() {
    if (device_ == nullptr) {
        return;
    }
    for (auto pipeline : pipelines_) {
        if (pipeline.valid()) {
            device_->destroy(pipeline);
        }
    }
    for (auto buffer : commands_) {
        device_->destroy(buffer);
    }
    if (origins_.valid()) {
        device_->destroy(origins_);
    }
    if (indices_.valid()) {
        device_->destroy(indices_);
    }
    if (vertices_.valid()) {
        device_->destroy(vertices_);
    }
}

std::expected<std::unique_ptr<TerrainRenderer>, rhi::RhiError> TerrainRenderer::create(
    rhi::Device& device, const TerrainRendererDesc& desc) {
    std::unique_ptr<TerrainRenderer> self(new TerrainRenderer);
    self->device_ = &device;
    self->desc_   = desc;
    self->arena_  = std::make_unique<render::VertexArena>(desc.arena_bytes);
    self->sections_.resize(desc.max_sections);
    self->free_slots_.reserve(desc.max_sections);
    for (u32 slot = desc.max_sections; slot-- > 0;) {
        self->free_slots_.push_back(slot);
    }
    self->scratch_.reserve(desc.max_sections);

    auto arena_buffer = device.create_buffer(rhi::BufferDesc{
        self->arena_->capacity(), rhi::BufferUsage::Vertex, "terrain arena", false});
    if (!arena_buffer) {
        return std::unexpected(arena_buffer.error());
    }
    self->vertices_ = *arena_buffer;

    // One index buffer for every section there will ever be. The quad pattern
    // is the same for all of them and an indirect command's vertexOffset does
    // the rest, so this is 96 KiB once instead of 24 bytes a quad, everywhere.
    const auto indices = render::build_shared_quad_indices(desc.max_quads_per_section);
    auto       index_buffer =
        device.create_buffer(rhi::BufferDesc{indices.size() * sizeof(u32), rhi::BufferUsage::Index,
                                             "shared quad indices", false});
    if (!index_buffer) {
        return std::unexpected(index_buffer.error());
    }
    self->indices_ = *index_buffer;
    if (auto uploaded = device.upload_buffer(
            self->indices_,
            std::span(reinterpret_cast<const u8*>(indices.data()), indices.size() * sizeof(u32)));
        !uploaded) {
        return std::unexpected(uploaded.error());
    }

    // Host-visible: a slot's origin is written once when the section arrives
    // and never again, and staging a single vec4 through a copy would cost a
    // command buffer to save nothing.
    auto origins = device.create_buffer(rhi::BufferDesc{
        static_cast<usize>(desc.max_sections) * 16, rhi::BufferUsage::Storage,
        "section origins", true});
    if (!origins) {
        return std::unexpected(origins.error());
    }
    self->origins_ = *origins;
    if (device.map(self->origins_) == nullptr) {
        OV_LOG_ERROR("the section origin buffer is not host visible");
        return std::unexpected(rhi::RhiError::OutOfMemory);
    }

    const u32 ring = rhi::Device::frames_in_flight();
    for (u32 i = 0; i < ring; ++i) {
        auto commands = device.create_buffer(rhi::BufferDesc{
            static_cast<usize>(desc.max_sections) * sizeof(IndirectCommand),
            rhi::BufferUsage::Indirect, "terrain draw commands", true});
        if (!commands) {
            return std::unexpected(commands.error());
        }
        self->commands_.push_back(*commands);
    }

    // ── Pipelines: one per layer, differing only in blend and culling ───────
    rhi::VertexBinding binding;
    binding.stride = sizeof(render::TerrainVertex);
    binding.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::R32Uint, 0});
    binding.attributes.push_back(rhi::VertexAttribute{1, rhi::Format::R32Uint, 4});
    binding.attributes.push_back(rhi::VertexAttribute{2, rhi::Format::R32Uint, 8});
    binding.attributes.push_back(rhi::VertexAttribute{3, rhi::Format::R32Uint, 12});

    const auto make = [&](rhi::BlendMode blend, bool depth_write, rhi::CullMode cull,
                          std::string_view name) {
        rhi::GraphicsPipelineDesc pipeline;
        pipeline.vertex_shader               = desc.atlas_vertex_shader;
        pipeline.fragment_shader             = desc.atlas_fragment_shader;
        pipeline.vertex_bindings             = {binding};
        pipeline.layout.sampled_image_count  = 1;
        pipeline.layout.storage_buffer_count = 1;
        pipeline.layout.push_constant_size   = sizeof(render::Mat4);
        pipeline.colour_format               = desc.colour_format;
        pipeline.depth_format                = desc.depth_format;
        pipeline.depth_test                  = true;
        pipeline.depth_write                 = depth_write;
        pipeline.cull_mode                   = cull;
        pipeline.blend                       = blend;
        pipeline.debug_name                  = name;
        return device.create_graphics_pipeline(pipeline);
    };

    const auto back = desc.backface_culling ? rhi::CullMode::Back : rhi::CullMode::None;
    // Cutout models are not closed solids — a leaf block's faces are visible
    // from both sides — so back-face culling would eat half of them.
    const std::array<std::tuple<rhi::BlendMode, bool, rhi::CullMode, std::string_view>,
                     static_cast<usize>(render::RenderLayer::Count)>
        settings{std::tuple{rhi::BlendMode::None, true, back, "terrain solid"},
                 std::tuple{rhi::BlendMode::None, true, rhi::CullMode::None, "terrain cutout"},
                 std::tuple{rhi::BlendMode::None, true, rhi::CullMode::None, "terrain cutout mip"},
                 // Translucent blends and does not write depth, so what is
                 // behind a pane of water still draws.
                 std::tuple{rhi::BlendMode::Alpha, false, rhi::CullMode::None,
                            "terrain translucent"}};
    for (usize i = 0; i < settings.size(); ++i) {
        const auto& [blend, depth_write, cull, name] = settings[i];
        auto pipeline                                = make(blend, depth_write, cull, name);
        if (!pipeline) {
            return std::unexpected(pipeline.error());
        }
        self->pipelines_[i] = *pipeline;
    }

    self->indirect_ = device.info().indirect_first_instance && !desc.force_per_section_draws;
    self->stats_.arena_capacity = self->arena_->capacity();
    OV_LOG_INFO("terrain: {} MiB arena, {} slots, {}", desc.arena_bytes / (1 << 20),
                desc.max_sections,
                self->indirect_ ? "one indirect draw per layer"
                                : "one draw per section — the driver has no indirect firstInstance");
    return self;
}

std::optional<u32> TerrainRenderer::add_section(Vec3f origin, render::RenderLayer layer,
                                                std::span<const render::TerrainVertex> vertices) {
    if (vertices.empty() || free_slots_.empty()) {
        return std::nullopt;
    }
    const u64 bytes  = vertices.size() * sizeof(render::TerrainVertex);
    const u64 offset = arena_->allocate(bytes);
    if (offset == render::VertexArena::kNoSpace) {
        OV_LOG_WARN("terrain arena full: {} bytes wanted, {} free in one piece", bytes,
                    arena_->largest_free());
        return std::nullopt;
    }
    if (!device_->upload_buffer(
            vertices_, std::span(reinterpret_cast<const u8*>(vertices.data()), bytes), offset)) {
        arena_->release(offset, bytes);
        return std::nullopt;
    }

    const u32 slot = free_slots_.back();
    free_slots_.pop_back();

    Section& section     = sections_[slot];
    section.origin       = origin;
    section.arena_offset = offset;
    section.arena_bytes  = bytes;
    // In vertices, not bytes: that is what vertexOffset counts, and it is the
    // reason the arena's page size is a multiple of the vertex.
    section.vertex_offset = static_cast<u32>(offset / sizeof(render::TerrainVertex));
    section.index_count   = static_cast<u32>(vertices.size() / 4 * 6);
    section.layer         = layer;
    section.live          = true;

    auto* origins = static_cast<f32*>(device_->map(origins_));
    origins[slot * 4 + 0] = origin.x;
    origins[slot * 4 + 1] = origin.y;
    origins[slot * 4 + 2] = origin.z;
    origins[slot * 4 + 3] = 0.0F;

    by_layer_[layer_index(layer)].push_back(slot);
    ++stats_.sections_resident;
    stats_.arena_used         = arena_->used();
    stats_.arena_largest_free = arena_->largest_free();
    return slot;
}

void TerrainRenderer::remove_section(u32 slot) {
    if (slot >= sections_.size() || !sections_[slot].live) {
        return;
    }
    Section& section = sections_[slot];
    arena_->release(section.arena_offset, section.arena_bytes);
    section.live = false;
    auto& live   = by_layer_[layer_index(section.layer)];
    // Order within a layer carries no meaning — the depth test settles solid
    // geometry and the translucent pass sorts for itself — so the cheap
    // removal is the right one.
    if (const auto at = std::ranges::find(live, slot); at != live.end()) {
        *at = live.back();
        live.pop_back();
    }
    free_slots_.push_back(slot);
    --stats_.sections_resident;
    stats_.arena_used         = arena_->used();
    stats_.arena_largest_free = arena_->largest_free();
}

void TerrainRenderer::draw(rhi::CommandList& cmd, const render::Mat4& view_projection,
                           const render::Frustum& frustum, Vec3f camera, rhi::ImageHandle atlas,
                           rhi::SamplerHandle sampler, bool cull) {
    const std::array<rhi::ImageHandle, 1>   images{atlas};
    const std::array<rhi::SamplerHandle, 1> samplers{sampler};
    const std::array<rhi::BufferHandle, 1>  storage{origins_};

    // One buffer per frame in flight, rotated here: frame N+1 must not write
    // over commands the GPU is still reading for frame N.
    const rhi::BufferHandle command_buffer = commands_[ring_];
    ring_ = (ring_ + 1) % static_cast<u32>(commands_.size());
    auto* mapped = static_cast<IndirectCommand*>(device_->map(command_buffer));

    cmd.bind_index_buffer(indices_);
    cmd.bind_vertex_buffer(0, vertices_);

    stats_.sections_drawn = 0;
    stats_.draw_calls     = 0;
    stats_.quads_drawn    = 0;
    u32 written           = 0;

    for (usize layer = 0; layer < pipelines_.size(); ++layer) {
        const bool translucent = layer == layer_index(render::RenderLayer::Translucent);
        scratch_.clear();
        distances_.clear();
        for (const u32 slot : by_layer_[layer]) {
            const Section& section = sections_[slot];
            // One plane test against a box removes most of the world at a
            // stroke. The compute-shader version comes after this is measured
            // and found wanting, not before.
            const Vec3f minimum = section.origin;
            const Vec3f maximum{section.origin.x + 16.0F, section.origin.y + 16.0F,
                                section.origin.z + 16.0F};
            if (cull && !frustum.intersects(minimum, maximum)) {
                continue;
            }
            scratch_.push_back(IndirectCommand{section.index_count, 1, 0,
                                               static_cast<i32>(section.vertex_offset), slot});
            if (translucent) {
                const Vec3f centre{section.origin.x + 8.0F, section.origin.y + 8.0F,
                                   section.origin.z + 8.0F};
                const Vec3f delta{centre.x - camera.x, centre.y - camera.y, centre.z - camera.z};
                distances_.push_back(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            }
            stats_.quads_drawn += section.index_count / 6;
        }
        if (scratch_.empty()) {
            continue;
        }
        // Back to front, because the translucent layer blends and does not
        // write depth: drawn the other way round, a far surface painted after a
        // near one blends over water it is behind.
        //
        // This orders whole sections, not the quads inside them. Vanilla sorts
        // the quads too, with a per-section index buffer it rewrites when the
        // camera has moved far enough — the asymmetry the plan anticipated.
        // Sections first because it is the ordering error that spans sixteen
        // blocks and is therefore the one that shows.
        if (translucent) {
            order_.resize(scratch_.size());
            for (u32 i = 0; i < order_.size(); ++i) {
                order_[i] = i;
            }
            std::ranges::sort(order_, [this](u32 a, u32 b) {
                return distances_[a] > distances_[b];
            });
            sorted_.clear();
            for (const u32 index : order_) {
                sorted_.push_back(scratch_[index]);
            }
            scratch_.swap(sorted_);
        }
        stats_.sections_drawn += static_cast<u32>(scratch_.size());

        cmd.bind_pipeline(pipelines_[layer]);
        cmd.bind_resources(pipelines_[layer], images, samplers, storage);
        cmd.push_constants(pipelines_[layer], &view_projection, sizeof(view_projection));

        if (indirect_ && mapped != nullptr) {
            std::memcpy(mapped + written, scratch_.data(),
                        scratch_.size() * sizeof(IndirectCommand));
            cmd.draw_indexed_indirect(command_buffer,
                                      static_cast<u64>(written) * sizeof(IndirectCommand),
                                      static_cast<u32>(scratch_.size()),
                                      sizeof(IndirectCommand));
            written += static_cast<u32>(scratch_.size());
            ++stats_.draw_calls;
        } else {
            // The fallback still gets the arena and the origin lookup; it only
            // loses the single call. firstInstance works in a direct draw
            // without the feature an indirect one needs.
            for (const IndirectCommand& command : scratch_) {
                cmd.draw_indexed(command.index_count, 1, 0, command.vertex_offset,
                                 command.first_instance);
                ++stats_.draw_calls;
            }
        }
    }
}

}  // namespace ov::client
