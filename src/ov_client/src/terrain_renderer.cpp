#define OV_LOG_CATEGORY "client"

#include "ov/client/terrain_renderer.hpp"

#include "ov/base/log.hpp"
#include "ov/render/translucent_sort.hpp"

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

/// Everything that is the same for every draw in a pass. 112 bytes, inside the
/// 128 every implementation guarantees.
struct TerrainPush {
    render::Mat4       view_projection;
    std::array<f32, 4> fog_colour;
    /// xyz the camera, w where the fog starts.
    std::array<f32, 4> camera_and_fog_start;
    /// x where the fog is complete. The rest is spare.
    std::array<f32, 4> fog_end;
};

static_assert(sizeof(TerrainPush) == 112, "the push constant block must fit the guaranteed 128");

[[nodiscard]] constexpr std::array<f32, 4> unpack_rgb(u32 colour) noexcept {
    return {static_cast<f32>((colour >> 16) & 0xFFU) / 255.0F,
            static_cast<f32>((colour >> 8) & 0xFFU) / 255.0F,
            static_cast<f32>(colour & 0xFFU) / 255.0F, 1.0F};
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
    for (auto buffer : sorted_indices_) {
        device_->destroy(buffer);
    }
    for (auto buffer : lightmap_staging_) {
        device_->destroy(buffer);
    }
    if (lightmap_sampler_.valid()) {
        device_->destroy(lightmap_sampler_);
    }
    if (lightmap_.valid()) {
        device_->destroy(lightmap_);
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

    // ── render-parity ── the translucent layer's sorted indices.
    self->sort_arena_ = std::make_unique<render::VertexArena>(kSortedIndexBytes);
    for (u32 i = 0; i < ring && ring <= 4; ++i) {
        auto sorted = device.create_buffer(rhi::BufferDesc{
            kSortedIndexBytes, rhi::BufferUsage::Index, "sorted translucent indices", true});
        if (!sorted) {
            return std::unexpected(sorted.error());
        }
        if (device.map(*sorted) == nullptr) {
            OV_LOG_WARN("sorted index buffer not host visible: translucent quads stay in mesh order");
            device.destroy(*sorted);
            break;
        }
        self->sorted_indices_.push_back(*sorted);
    }

    // ── The lightmap ────────────────────────────────────────────────────────
    //
    // Sixteen by sixteen, rebuilt on the CPU every frame and sampled at
    // (block light, sky light). This is vanilla's own mechanism rather than a
    // formula in the terrain shader, and the reason to keep it is that the
    // things which will move next — a torch's flicker, the brightness slider,
    // a sunset — are all changes to this one small image.
    auto lightmap = device.create_image(rhi::ImageDesc{render::Lightmap::kSize,
                                                       render::Lightmap::kSize, 1,
                                                       rhi::Format::Rgba8Unorm, true, false,
                                                       "lightmap"});
    if (!lightmap) {
        return std::unexpected(lightmap.error());
    }
    self->lightmap_ = *lightmap;
    // Unorm and not Srgb: these are light multipliers, not colours to be
    // displayed, and putting them through a transfer function would darken
    // every mid-tone in the world.

    // Linear, unlike the atlas. The lightmap is a gradient sampled between
    // texels on purpose — it is what makes light fall off smoothly across a
    // face instead of in sixteen steps.
    auto lightmap_sampler = device.create_sampler(
        rhi::SamplerDesc{rhi::Filter::Linear, rhi::Filter::Linear, rhi::MipFilter::Nearest,
                         rhi::AddressMode::ClampToEdge, 1.0F, 0.0F});
    if (!lightmap_sampler) {
        return std::unexpected(lightmap_sampler.error());
    }
    self->lightmap_sampler_ = *lightmap_sampler;

    for (u32 i = 0; i < ring; ++i) {
        auto staging = device.create_buffer(rhi::BufferDesc{
            render::Lightmap::kBytes, rhi::BufferUsage::Upload, "lightmap staging", true});
        if (!staging) {
            return std::unexpected(staging.error());
        }
        self->lightmap_staging_.push_back(*staging);
    }

    // The image starts undefined, and the first frame transitions it from
    // there. Giving it content now means a frame that somehow skips the upload
    // shows a black world rather than reading uninitialised memory.
    {
        render::Lightmap initial;
        initial.update(1.0F, render::kOverworldAmbientLight, 0.5F, 0.0F);
        if (auto uploaded = device.upload_image(self->lightmap_, initial.pixels(),
                                                render::Lightmap::kSize, render::Lightmap::kSize);
            !uploaded) {
            return std::unexpected(uploaded.error());
        }
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
        // Two: the block atlas and the lightmap.
        pipeline.layout.sampled_image_count  = 2;
        pipeline.layout.storage_buffer_count = 1;
        pipeline.layout.push_constant_size   = sizeof(TerrainPush);
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
    // ── render-parity ── a translucent section keeps its quad centres and a
    // range of the sorted buffers; the first frame writes it in mesh order.
    section.centres.clear();
    section.order.clear();
    section.sorted_from = Vec3f{1.0e30F, 1.0e30F, 1.0e30F};
    section.sort_offset = render::VertexArena::kNoSpace;
    section.stale.fill(true);
    if (layer == render::RenderLayer::Translucent && !sorted_indices_.empty()) {
        render::quad_centres(vertices, section.centres);
        section.sort_offset = sort_arena_->allocate(static_cast<u64>(section.index_count) * 4);
        if (section.sort_offset == render::VertexArena::kNoSpace) {
            OV_LOG_WARN("sorted index buffer full: a translucent section stays in mesh order");
        }
    }

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
    if (section.sort_offset != render::VertexArena::kNoSpace) {  // ── render-parity ──
        sort_arena_->release(section.sort_offset, static_cast<u64>(section.index_count) * 4);
        section.sort_offset = render::VertexArena::kNoSpace;
    }
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

void TerrainRenderer::upload_sky(rhi::CommandList& cmd, const render::Lightmap& lightmap) {
    const rhi::BufferHandle staging = lightmap_staging_[device_->frame_index()];
    if (!device_->write_buffer(staging, lightmap.pixels().data(), lightmap.pixels().size())) {
        return;
    }
    // Undefined rather than ShaderRead as the source state: the previous
    // contents are of no interest, and telling the driver so lets it skip
    // preserving them.
    cmd.transition(lightmap_, rhi::ResourceState::Undefined, rhi::ResourceState::TransferDest);
    cmd.copy_buffer_to_image(staging, 0, lightmap_, 0, render::Lightmap::kSize,
                             render::Lightmap::kSize);
    cmd.transition(lightmap_, rhi::ResourceState::TransferDest, rhi::ResourceState::ShaderRead);
}

void TerrainRenderer::draw(rhi::CommandList& cmd, const render::Mat4& view_projection,
                           const render::Frustum& frustum, Vec3f camera, rhi::ImageHandle atlas,
                           rhi::SamplerHandle sampler, const SkyFrame& sky, bool cull) {
    const std::array<rhi::ImageHandle, 2>   images{atlas, lightmap_};
    const std::array<rhi::SamplerHandle, 2> samplers{sampler, lightmap_sampler_};
    const std::array<rhi::BufferHandle, 1>  storage{origins_};

    TerrainPush push;
    push.view_projection      = view_projection;
    push.fog_colour           = unpack_rgb(sky.fog_colour);
    push.camera_and_fog_start = {camera.x, camera.y, camera.z, sky.fog_start};
    push.fog_end              = {sky.fog_end, 0.0F, 0.0F, 0.0F};

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
        // ── render-parity ── the quads of each translucent section back to
        // front, from its own index range.
        const bool sorted_quads = translucent && prepare_translucent(camera);
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
        cmd.push_constants(pipelines_[layer], &push, sizeof(push));
        if (sorted_quads) {
            cmd.bind_index_buffer(sorted_indices_[device_->frame_index() % sorted_indices_.size()]);
        } else if (translucent) {
            // Mesh order: the commands still point at the shared pattern.
            for (IndirectCommand& command : scratch_) {
                command.first_index = 0;
            }
        }

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
                cmd.draw_indexed(command.index_count, 1, command.first_index,
                                 command.vertex_offset, command.first_instance);
                ++stats_.draw_calls;
            }
        }
    }
}

bool TerrainRenderer::prepare_translucent(Vec3f camera) {
    if (sorted_indices_.empty()) {
        return false;
    }
    const usize ring   = device_->frame_index() % sorted_indices_.size();
    auto*       mapped = static_cast<u8*>(device_->map(sorted_indices_[ring]));
    if (mapped == nullptr) {
        return false;
    }

    // Which sections want a new order: the eye has moved a block from where
    // they were last sorted. Nearest first, and only so many a frame.
    resort_.clear();
    for (const IndirectCommand& command : scratch_) {
        const Section& section = sections_[command.first_instance];
        if (section.sort_offset == render::VertexArena::kNoSpace) {
            return false;
        }
        const Vec3f eye{camera.x - section.origin.x, camera.y - section.origin.y,
                        camera.z - section.origin.z};
        const Vec3f moved{eye.x - section.sorted_from.x, eye.y - section.sorted_from.y,
                          eye.z - section.sorted_from.z};
        if (moved.x * moved.x + moved.y * moved.y + moved.z * moved.z >= 1.0F) {
            resort_.push_back(command.first_instance);
        }
    }
    const auto distance_to = [&](u32 slot) {
        const Section& s = sections_[slot];
        const f32 dx = s.origin.x + 8.0F - camera.x;
        const f32 dy = s.origin.y + 8.0F - camera.y;
        const f32 dz = s.origin.z + 8.0F - camera.z;
        return dx * dx + dy * dy + dz * dz;
    };
    if (resort_.size() > kResortsPerFrame) {
        std::ranges::partial_sort(resort_, resort_.begin() + kResortsPerFrame,
                                  [&](u32 a, u32 b) { return distance_to(a) < distance_to(b); });
        resort_.resize(kResortsPerFrame);
    }
    for (const u32 slot : resort_) {
        Section&    section = sections_[slot];
        const Vec3f eye{camera.x - section.origin.x, camera.y - section.origin.y,
                        camera.z - section.origin.z};
        render::sort_back_to_front(section.centres, eye, section.order);
        section.sorted_from = eye;
        section.stale.fill(true);
    }

    // This frame's buffer, wherever its copy of a range is out of date. The
    // other frame's copy is written when its turn comes, never while the GPU
    // may still be reading it.
    for (IndirectCommand& command : scratch_) {
        Section& section = sections_[command.first_instance];
        if (section.stale[ring]) {
            if (section.order.empty()) {
                section.order.resize(section.centres.size());
                for (u32 i = 0; i < section.order.size(); ++i) {
                    section.order[i] = i;
                }
            }
            render::write_quad_indices(section.order, sort_scratch_);
            std::memcpy(mapped + section.sort_offset, sort_scratch_.data(),
                        sort_scratch_.size() * sizeof(u32));
            section.stale[ring] = false;
        }
        command.first_index = static_cast<u32>(section.sort_offset / sizeof(u32));
    }
    return true;
}

}  // namespace ov::client
