// A column of the world: 16 x 16 blocks, floor to sky.
//
// The unit the game loads, saves, sends, generates and ticks. A chunk owns its
// sections, its heightmaps and its biomes; it does not know about the tick, the
// network or entities, which is what lets the authoritative world and the
// client's replica share this type instead of maintaining two of them.
//
// Vertical coordinates are **world y**, not an index. A chunk that reported
// positions relative to its own floor would be correct and would force every
// caller to convert, and one caller forgetting is a structure sixty-four blocks
// underground.
#pragma once

#include "ov/math/block_pos.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/world/chunk_section.hpp"
#include "ov/world/heightmap.hpp"

#include <array>
#include <vector>

namespace ov::world {

/// The shape of a dimension's vertical extent.
///
/// Passed in rather than assumed: the Nether and the End are 256 blocks from 0,
/// the Overworld 384 from -64, and a datapack dimension can be anything. Code
/// that hard-codes -64 works until the first Nether chunk.
struct WorldShape {
    i32 min_y{-64};
    u32 height{384};

    [[nodiscard]] static constexpr WorldShape overworld() noexcept { return {-64, 384}; }

    [[nodiscard]] static constexpr WorldShape nether() noexcept { return {0, 256}; }

    [[nodiscard]] static constexpr WorldShape the_end() noexcept { return {0, 256}; }

    [[nodiscard]] constexpr i32 max_y() const noexcept {
        return min_y + static_cast<i32>(height) - 1;
    }

    [[nodiscard]] constexpr usize section_count() const noexcept { return height / 16; }

    /// The section coordinate the world's floor sits in: -4 for the Overworld.
    [[nodiscard]] constexpr i32 min_section() const noexcept { return min_y >> 4; }

    [[nodiscard]] constexpr bool contains_y(i32 y) const noexcept {
        return y >= min_y && y <= max_y();
    }
};

/// The heightmaps a chunk keeps once it is generated.
///
/// The `_WG` pair exists only while a chunk is being generated and is dropped
/// before it is stored, so it lives in the generator's working state rather
/// than here.
inline constexpr usize kStoredHeightmapCount = 4;

/// A block that carries data the block state cannot hold: a sign's text, a
/// chest's contents, a spawner's mob.
///
/// Kept beside the sections rather than in them. A block entity is rare — a
/// handful per chunk against 98304 blocks — and giving every cell room for one
/// would cost more than the world itself.
struct BlockEntity {
    /// Chunk-local x and z, world y — the same convention as everything else
    /// here, so a caller never has to remember which of the three is different.
    u8  x{0};
    i32 y{0};
    u8  z{0};

    /// The registry name, e.g. "minecraft:sign". Disk is name-based, so this is
    /// what gets written there.
    std::string type;

    /// The same thing as a number, which is what the wire carries. Both are
    /// kept because converting needs the registry and this struct does not have
    /// one — and a block entity read from disk has to be sendable without one.
    i32 type_id{0};

    /// Everything else, as the format stores it.
    nbt::Tag data;
};

class Chunk {
public:
    /// `blocks` is what makes the four heightmaps maintainable: MOTION_BLOCKING
    /// and OCEAN_FLOOR need to know whether a block stops movement, which is
    /// measured data living in the registry. It has no default — a chunk built
    /// without one keeps only WORLD_SURFACE current, and that has to be a
    /// decision at the call site rather than an oversight.
    Chunk(ChunkPos position, WorldShape shape, AirStates air,
          const registry::BlockRegistry* blocks);

    [[nodiscard]] ChunkPos position() const noexcept { return position_; }

    [[nodiscard]] WorldShape shape() const noexcept { return shape_; }

    /// Sections, bottom first. Always the full column, empty ones included:
    /// the chunk packet sends every section whether or not it holds anything.
    [[nodiscard]] std::span<const ChunkSection> sections() const noexcept { return sections_; }

    /// The section holding world y, or nullptr outside the world.
    [[nodiscard]] ChunkSection*       section_for_y(i32 y) noexcept;
    [[nodiscard]] const ChunkSection* section_for_y(i32 y) const noexcept;

    /// Read a block at chunk-local x and z, world y.
    [[nodiscard]] registry::BlockStateId get_block(usize x, i32 y, usize z) const noexcept;

    /// Place a block, keeping the heightmaps current.
    ///
    /// Raising is a comparison. Lowering — breaking the block that *was* the
    /// surface — needs a scan back down the column, which is why breaking is
    /// the expensive direction and why it is done here rather than left to
    /// callers to remember.
    void set_block(usize x, i32 y, usize z, registry::BlockStateId state);

    [[nodiscard]] u16 get_biome(usize x, i32 y, usize z) const noexcept;
    void              set_biome(usize x, i32 y, usize z, u16 biome);

    /// Make the whole column one biome.
    ///
    /// Not the same as setting all 64 cells of every section: a palette never
    /// shrinks, so writing over a container cell by cell leaves the value it
    /// started with in the palette forever. That is harmless but it is not what
    /// vanilla sends, and "all one biome" is a common enough case — a flat
    /// world, a chunk well inside a biome — to deserve saying directly.
    void fill_biome(u16 biome);

    [[nodiscard]] Heightmap&       heightmap(HeightmapType type) noexcept;
    [[nodiscard]] const Heightmap& heightmap(HeightmapType type) const noexcept;

    /// Rebuild every heightmap by scanning each column down.
    ///
    /// What a freshly loaded or generated chunk needs once; maintaining them
    /// incrementally afterwards is what `set_block` does. The stored maps are
    /// never trusted: a file written by another implementation can carry any
    /// numbers at all, and a wrong heightmap is invisible until rain falls
    /// inside the ground.
    void recompute_heightmaps() noexcept;

    /// Non-air blocks across the whole column.
    [[nodiscard]] usize non_air_count() const noexcept;

    [[nodiscard]] std::span<const BlockEntity> block_entities() const noexcept {
        return block_entities_;
    }

    /// The block entity at a position, or nullptr.
    [[nodiscard]] BlockEntity*       block_entity_at(usize x, i32 y, usize z) noexcept;
    [[nodiscard]] const BlockEntity* block_entity_at(usize x, i32 y, usize z) const noexcept;

    /// Add or replace the block entity at a position.
    void set_block_entity(BlockEntity entity);

    /// Remove it, if there is one. Called when the block itself changes: a
    /// block entity outliving its block is a chest that cannot be opened and
    /// cannot be removed.
    void remove_block_entity(usize x, i32 y, usize z);

private:
    [[nodiscard]] usize section_index_for_y(i32 y) const noexcept;
    [[nodiscard]] i32   scan_surface_down(usize x, usize z, i32 from_y) const noexcept;

    /// Does this state count towards the given heightmap?
    ///
    /// The four predicates, measured on a real 1.20.1 server rather than
    /// guessed: WORLD_SURFACE takes anything that is not air, OCEAN_FLOOR
    /// anything that stops movement, MOTION_BLOCKING that plus anything holding
    /// a fluid, and MOTION_BLOCKING_NO_LEAVES the same minus the ten leaf
    /// blocks.
    [[nodiscard]] bool counts_for(HeightmapType type, registry::BlockStateId state) const noexcept;

    /// Bring one heightmap in line with a block that just changed.
    void update_heightmap(HeightmapType type, usize x, i32 y, usize z,
                          registry::BlockStateId state) noexcept;

    /// Scan down for the highest y at or below `from_y` counting for `type`.
    [[nodiscard]] i32 scan_down(HeightmapType type, usize x, usize z, i32 from_y) const noexcept;

    ChunkPos                       position_;
    WorldShape                     shape_;
    AirStates                      air_;
    const registry::BlockRegistry* blocks_{nullptr};
    std::vector<ChunkSection>      sections_;

    /// Indexed by HeightmapType for the four stored kinds.
    std::array<Heightmap, kStoredHeightmapCount> heightmaps_;

    /// Unordered: there are a handful per chunk, and keeping them sorted would
    /// cost more than the linear scan it saves.
    std::vector<BlockEntity> block_entities_;
};

}  // namespace ov::world
