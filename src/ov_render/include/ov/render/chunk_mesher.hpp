// Meshing a real chunk section, out of a real world.
//
// The demo scene this replaces answered `occludes` with "is the block solid",
// which is wrong for everything that is not a full cube: a slab would have
// hidden the ground under it. The registry answers it exactly — every state's
// collision shape is compiled into the pack, with a precomputed mask of which
// faces present a full square — and that mask is checked: the fence connection
// rule rebuilt from it reproduces 23358 of 23358 faces measured on a real
// 1.20.1 server.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/render/biome_colours.hpp"
#include "ov/render/block_models.hpp"
#include "ov/render/mesher.hpp"
#include "ov/world/chunk.hpp"

#include <array>

namespace ov::render {

/// The nine chunks a section needs to mesh: its own and the eight around it.
///
/// Indexed `(dz + 1) * 3 + (dx + 1)`, so the centre is 4. A null entry is
/// treated as air, which is what a chunk at the edge of what has been loaded
/// should look like — the alternative, treating it as solid, walls the world in
/// with invisible surfaces.
using ChunkNeighbours = std::array<const world::Chunk*, 9>;

/// Turns a world's own biome ids into the colours a biome paints with.
///
/// Two indirections and not one, because they are genuinely different things:
/// a chunk stores whatever numbering its own palette used when it was loaded,
/// while BiomeColours is indexed by the registry's order. A world can carry a
/// biome the registry has never heard of — a datapack's, or a modded one — and
/// that has to come back as something ordinary rather than as an out-of-range
/// read.
class BiomeTints {
public:
    BiomeTints(const BiomeColours& colours, std::span<const u32> world_to_registry) noexcept
        : colours_(&colours), world_to_registry_(world_to_registry) {}

    [[nodiscard]] Rgb colour(u16 world_biome, TintChannel channel) const noexcept;

private:
    const BiomeColours*  colours_;
    std::span<const u32> world_to_registry_;
};

/// A NeighbourhoodView over a 3x3 of chunks, addressed in section-local
/// coordinates so that the packed vertex's range is the natural one.
class ChunkSectionView final : public NeighbourhoodView {
public:
    ChunkSectionView(const registry::BlockRegistry& blocks, const ChunkNeighbours& chunks,
                     i32 origin_x, i32 origin_y, i32 origin_z,
                     const BiomeTints* tints = nullptr) noexcept;

    /// The state at a section-local position, or air outside what is loaded.
    [[nodiscard]] registry::BlockStateId state_at(Vec3i local) const noexcept;

    [[nodiscard]] bool occludes(Vec3i position, Direction towards) const override;
    [[nodiscard]] bool casts_ambient_occlusion(Vec3i position) const override;
    [[nodiscard]] f32  ao_shade(Vec3i position) const override;
    [[nodiscard]] bool blocks_view(Vec3i position) const override;
    [[nodiscard]] u8   sky_light(Vec3i position) const override;
    [[nodiscard]] u8   block_light(Vec3i position) const override;
    [[nodiscard]] u16  fluid_at(Vec3i position) const override;
    [[nodiscard]] u32  biome_colour(Vec3i position, TintChannel channel) const override;

    /// True when no section in range carried any stored light.
    ///
    /// Worth knowing rather than guessing: a world saved without light renders
    /// pitch black, and "the mesher is broken" and "the file has no light in
    /// it" look identical on screen.
    [[nodiscard]] bool any_light_stored() const noexcept { return light_seen_; }

    /// The world position of a section-local one: what the position random
    /// of a block's model alternatives is seeded from.
    [[nodiscard]] Vec3i world_position(Vec3i local) const noexcept {
        return Vec3i{origin_x_ + local.x, origin_y_ + local.y, origin_z_ + local.z};
    }

    /// ── implicit water ── the water source drawn *with* this block, or air.
    ///
    /// A block that holds water — `waterlogged=true`, or seagrass, tall
    /// seagrass, kelp, kelp_plant and a bubble column, which always do — draws
    /// its model and the water it stands in. Air for the water block itself,
    /// which draws its own.
    [[nodiscard]] registry::BlockStateId held_water(Vec3i local) const noexcept;

private:
    [[nodiscard]] const world::Chunk* chunk_for(i32 world_x, i32 world_z) const noexcept;

    const registry::BlockRegistry* blocks_;
    const BiomeTints*              tints_{nullptr};
    ChunkNeighbours                chunks_;
    i32                            origin_x_;
    i32                            origin_y_;
    i32                            origin_z_;
    mutable bool                   light_seen_{false};
    /// ── implicit water ── minecraft:water[level=0], resolved once.
    registry::BlockStateId water_source_{0};
};

struct SectionMeshStats {
    usize blocks_visited{0};
    usize blocks_drawn{0};
    usize quads{0};
};

/// Mesh one 16x16x16 section. Positions come out section-local, which is what
/// the vertex format is scaled for; the section's world origin goes in the
/// draw's push constants.
[[nodiscard]] SectionMeshStats mesh_section(const ChunkSectionView& view, BlockModelCache& models,
                                            const TextureAtlas& atlas, MeshBuffers& out);

}  // namespace ov::render
