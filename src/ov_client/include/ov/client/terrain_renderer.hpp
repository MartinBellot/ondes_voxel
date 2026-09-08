// The terrain, drawn in three calls instead of several thousand.
//
// Every section's vertices live in one device-local arena, so a draw never
// rebinds a vertex buffer. What used to be pushed per draw — the section's
// origin — is looked up instead, from a storage buffer indexed by the command's
// own firstInstance, which is the one per-draw channel an indirect command has
// that reaches the vertex shader. What is left is a frustum cull that writes
// VkDrawIndexedIndirectCommand records and one vkCmdDrawIndexedIndirect per
// layer.
//
// This lives in ov_client rather than ov_render for a reason worth keeping:
// ov_render has no GPU in it at all, which is what lets the model pipeline,
// the mesher and the arena's free list be unit tests instead of screenshots.
// Only the module that already owns the window and the device owns the
// buffers.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/frustum.hpp"
#include "ov/render/mesher.hpp"
#include "ov/render/terrain_vertex.hpp"
#include "ov/render/vertex_arena.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ov::client {

struct TerrainRendererDesc {
    /// The device-local arena, in bytes.
    ///
    /// The plan said 384 MiB and that was right for a 12-byte vertex: 259 MiB
    /// measured at a radius of 12. The vertex then grew to 16 bytes to carry
    /// the baked biome colour, the same world became 340 MiB, and 44 MiB of
    /// headroom is not enough to walk around in. 512 keeps roughly the
    /// proportion the plan intended. The renderer reports what it actually
    /// uses, so this stays a number that was measured rather than chosen.
    u64 arena_bytes{512ULL * 1024 * 1024};
    /// The most sections that can be resident at once. Bounds the origin
    /// table and the indirect command buffers, both of which are indexed by
    /// slot.
    u32 max_sections{16384};
    /// The largest number of quads any one section can hold, which fixes the
    /// length of the one index buffer they all share.
    u32 max_quads_per_section{4096};

    std::string_view atlas_vertex_shader{"terrain.vert.spv"};
    std::string_view atlas_fragment_shader{"terrain.frag.spv"};
    rhi::Format      colour_format{rhi::Format::Bgra8Srgb};
    rhi::Format      depth_format{rhi::Format::Depth32Float};
    /// Diagnostics, passed through from the command line.
    bool backface_culling{true};
    /// Record one draw per section even when the driver supports indirect.
    ///
    /// This exists to make the claim checkable. "Indirect draws made it
    /// faster" is worth nothing unless the two paths can be run against the
    /// same world, the same camera and the same arena on the same afternoon,
    /// with only the draw submission different.
    bool force_per_section_draws{false};
};

/// What the last recorded frame cost, in objects rather than time.
struct TerrainStats {
    u32 sections_resident{0};
    u32 sections_drawn{0};
    u32 draw_calls{0};
    u64 arena_used{0};
    u64 arena_capacity{0};
    u64 arena_largest_free{0};
    /// Quads submitted last frame. The GPU-side cost, as opposed to the
    /// CPU-side one the draw call count measures.
    u64 quads_drawn{0};
};

class TerrainRenderer {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<TerrainRenderer>, rhi::RhiError> create(
        rhi::Device& device, const TerrainRendererDesc& desc);

    TerrainRenderer(const TerrainRenderer&)            = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;
    ~TerrainRenderer();

    /// Copy one section's vertices into the arena and give it a slot.
    ///
    /// Nullopt means the arena had no room, or every slot is taken. It is a
    /// normal outcome at the edge of a view distance, not an error, so the
    /// caller decides what to drop.
    [[nodiscard]] std::optional<u32> add_section(Vec3f origin, render::RenderLayer layer,
                                                 std::span<const render::TerrainVertex> vertices);

    /// Give a slot's arena range back. The slot number may be reused at once.
    void remove_section(u32 slot);

    /// Cull, write the commands, and record the draws. Must be called inside a
    /// begin_rendering / end_rendering pair.
    void draw(rhi::CommandList& cmd, const render::Mat4& view_projection,
              const render::Frustum& frustum, Vec3f camera, rhi::ImageHandle atlas,
              rhi::SamplerHandle sampler, bool cull = true);

    [[nodiscard]] const TerrainStats& stats() const noexcept { return stats_; }
    /// False when the driver made us fall back to a draw per section.
    [[nodiscard]] bool indirect() const noexcept { return indirect_; }

private:
    struct Section {
        Vec3f               origin;
        u64                 arena_offset{0};
        u64                 arena_bytes{0};
        u32                 vertex_offset{0};
        u32                 index_count{0};
        render::RenderLayer layer{render::RenderLayer::Solid};
        bool                live{false};
    };

    /// Mirrors VkDrawIndexedIndirectCommand exactly. Written here rather than
    /// included, because naming the Vulkan type in a public header of any
    /// module is the one thing ov_rhi exists to prevent — and the layout is
    /// fixed by the specification, so a static_assert in the .cpp is enough to
    /// keep the two honest.
    struct IndirectCommand {
        u32 index_count{0};
        u32 instance_count{1};
        u32 first_index{0};
        i32 vertex_offset{0};
        u32 first_instance{0};
    };

    TerrainRenderer() = default;

    rhi::Device*        device_{nullptr};
    TerrainRendererDesc desc_;

    std::unique_ptr<render::VertexArena> arena_;
    rhi::BufferHandle                    vertices_;
    rhi::BufferHandle                    indices_;
    /// vec4 per slot: xyz is the origin, w pads to the std430 alignment a vec4
    /// array demands.
    rhi::BufferHandle              origins_;
    std::vector<rhi::BufferHandle> commands_;

    std::array<rhi::PipelineHandle, static_cast<usize>(render::RenderLayer::Count)> pipelines_{};

    std::vector<Section> sections_;
    std::vector<u32>     free_slots_;
    /// The live slots of each layer, so that culling walks the sections that
    /// exist rather than the slots that might. With a table sized for the
    /// worst case that is the difference between a hundred thousand iterations
    /// a frame and ten thousand, and it does not show up as a draw call.
    std::array<std::vector<u32>, static_cast<usize>(render::RenderLayer::Count)> by_layer_{};
    /// Scratch, reused every frame. Nothing here allocates once the world has
    /// settled, which is the same rule the tick obeys.
    std::vector<IndirectCommand> scratch_;
    /// Squared distance from the camera to each command in `scratch_`, for the
    /// translucent layer's ordering. Parallel rather than a member of the
    /// command, because the command's layout is fixed by Vulkan.
    std::vector<f32> distances_;
    std::vector<u32> order_;
    /// Members, not locals: this runs every frame, and a vector built inside
    /// the loop would allocate in the hot path.
    std::vector<IndirectCommand> sorted_;

    u32          ring_{0};
    bool         indirect_{false};
    TerrainStats stats_;
};

}  // namespace ov::client
