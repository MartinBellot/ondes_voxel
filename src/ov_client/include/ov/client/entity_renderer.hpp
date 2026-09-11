// Everything that moves, drawn: mobs, players, dropped stacks, chests, signs.
//
// This is deliberately the *thin* half. All the geometry — the bone hierarchy,
// the box nets, the walk cycle, the model-to-world reflection — happens in
// ov_render with no GPU in sight, which is what lets `test_ov_render` assert
// that a zombie's box is 0.6 by 1.95 blocks. What is left here is a vertex
// ring, a texture table and a draw per texture change.
//
// Why the vertices are rebuilt every frame rather than posed on the GPU. The
// alternative is a bone matrix palette in a storage buffer and a vertex shader
// that walks it, which is faster and untestable: a mob posed wrongly by a
// shader is a screenshot, not an assertion. A hundred visible mobs cost about
// half a megabyte of writes a frame; § "Ce que les entités coûtent" of
// docs/provenance/rendu-entites.md says what that measured, and the number was
// small enough that the trade needed no further defending.
//
// ⚠ The descriptor pool is 64 sets a frame (ov_rhi), and this cost a
// measurement to learn. A first version cut a batch whenever the *submitted*
// texture changed, which for a hundred mobs of eight species in map order is a
// hundred and ten batches: the pool ran out at sixty-four, `rhi` logged
// "descriptor pool exhausted this frame", and the rest drew with whatever set
// was last bound. So a submission is filed into a bucket per texture and the
// buckets are concatenated at draw time. The draw count is therefore bounded by
// the number of textures — ten — whatever the number of entities, and it is in
// the stats so that a regression is one line of output rather than a screenshot
// of a zombie wearing a cow.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"
#include "ov/render/camera.hpp"
#include "ov/render/entity_mesh.hpp"
#include "ov/render/texture_image.hpp"
#include "ov/rhi/device.hpp"

#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ov::client {

/// An uploaded entity texture. An index into the renderer's own table, never an
/// rhi handle, so nothing above this file names a GPU type.
enum class EntityTexture : u16 { Invalid = 0xFFFF };

/// What one frame's entities cost, in objects rather than in time.
struct EntityStats {
    u32 entities{0};
    u32 quads{0};
    u32 draws{0};
    u32 vertices{0};
    /// High-water mark since creation, so a ring that is nearly full is visible
    /// before it overflows.
    u32 peak_vertices{0};
    /// Entities dropped this frame because the ring was full.
    u32 dropped{0};
};

/// The fog and sky the entities share with the terrain.
///
/// Passed rather than recomputed: a mob lit or fogged differently from the
/// ground it stands on is the single most visible way for an entity renderer to
/// be wrong, and it is wrong by one struct not being shared.
struct EntitySky {
    /// 0xRRGGBB, already faded for the time of day.
    u32 fog_colour{0xC0D8FF};
    f32 fog_start{0.0F};
    f32 fog_end{192.0F};
};

/// Which pass a renderer draws. ── weather ── The translucent one is the rain's
/// and the snow's: the same vertices and push block, blended rather than cut
/// out, drawn both sides, never writing depth, with a repeating sampler so a
/// sheet's texture can scroll.
///
/// ── entity-models ── The last three are the game's other entity render
/// types: `entity_translucent` (a slime's jelly, a horse's markings: blended,
/// depth written), `eyes` (added, unlit, depth not written) and
/// `energy_swirl` (the same, on a repeating texture that scrolls). Every entity
/// pass compares depth less-or-equal, as the game does: a villager's clothes
/// are the same triangles as its skin, drawn again on top, and a strict test
/// would drop every one of them.
enum class EntityPass : u8 { Cutout, Translucent, EntityTranslucent, Eyes, Energy };

class EntityRenderer {
public:
    /// Quads per frame in flight. A hundred mobs of a dozen cubes each is about
    /// seven thousand; this leaves room for the signs and the dropped stacks
    /// and still costs three megabytes a ring slot.
    static constexpr u32 kMaxQuads = 32768;

    [[nodiscard]] static std::expected<std::unique_ptr<EntityRenderer>, rhi::RhiError> create(
        rhi::Device& device, rhi::Format colour_format, rhi::Format depth_format,
        EntityPass pass = EntityPass::Cutout);

    EntityRenderer(const EntityRenderer&)            = delete;
    EntityRenderer& operator=(const EntityRenderer&) = delete;
    ~EntityRenderer();

    /// Upload a texture and keep it for the life of the renderer.
    ///
    /// One image per entity texture rather than one atlas. An atlas would need
    /// padding these textures do not have — a model addresses its sheet at
    /// texel granularity with no border — and a 64x32 mob sheet dropped into
    /// the block atlas would drag its mip level down for the whole world.
    [[nodiscard]] std::expected<EntityTexture, rhi::RhiError> add_texture(
        const render::TextureImage& image, std::string_view name);

    /// Reference an image somebody else owns, and must not be destroyed here.
    ///
    /// The block atlas is the reason this exists: a dropped stack is drawn from
    /// the sprites the terrain already uploaded, and uploading them twice would
    /// double a hundred megabytes to save one indirection.
    [[nodiscard]] EntityTexture borrow_texture(rhi::ImageHandle image, u32 width, u32 height);

    /// ── entity-models ── The image behind a texture, so another pass can
    /// borrow it: the entity atlas is uploaded once and read by the cutout,
    /// translucent and eyes passes alike. An invalid handle for a texture this
    /// renderer does not have.
    [[nodiscard]] rhi::ImageHandle image(EntityTexture texture) const noexcept;

    /// Start a frame. Clears the batches; allocates nothing once warm.
    void begin();

    /// Append one entity's quads, already in world space.
    ///
    /// A batch is cut only when the texture changes, so a caller that groups
    /// its entities by species pays one draw a species. Returns false when the
    /// ring is full, which is a dropped entity rather than an error.
    bool submit(EntityTexture texture, std::span<const render::EntityVertex> vertices);

    /// Record every batch. Inside a render pass, after the terrain.
    void draw(rhi::CommandList& cmd, const render::Mat4& view_projection, Vec3f camera,
              const EntitySky& sky);

    [[nodiscard]] const EntityStats& stats() const noexcept { return stats_; }

private:
    struct Batch {
        rhi::ImageHandle image;
        /// First quad and quad count, not vertices: the index buffer is shared
        /// and addressed in quads.
        u32 first{0};
        u32 count{0};
    };

    struct Texture {
        rhi::ImageHandle image;
        u32              width{0};
        u32              height{0};
        /// False for a borrowed image, which somebody else destroys.
        bool owned{false};
        /// This frame's vertices for this texture. A member rather than a local
        /// so that a frame allocates nothing after the first: it is cleared,
        /// never freed.
        std::vector<render::EntityVertex> pending;
    };

    EntityRenderer() = default;

    rhi::Device*        device_{nullptr};
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle  sampler_;
    /// 0, 1, 2, 0, 2, 3 for every quad the ring can hold. One buffer, built
    /// once: every quad in every model has the same winding.
    rhi::BufferHandle indices_;

    std::vector<Texture>           textures_;
    std::vector<rhi::BufferHandle> vertices_;
    u32                            ring_{0};

    /// Every bucket, concatenated in texture order, ready to upload.
    std::vector<render::EntityVertex> scratch_;
    std::vector<Batch>                batches_;
    /// Total quads filed this frame, so that submit() can refuse before a
    /// bucket overflows the ring rather than after.
    u32 pending_quads_{0};

    EntityStats stats_;
};

}  // namespace ov::client
