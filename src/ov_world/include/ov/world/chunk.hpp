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

class Chunk {
public:
    Chunk(ChunkPos position, WorldShape shape, AirStates air);

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

    /// Place a block, keeping WORLD_SURFACE current.
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

    /// Rebuild WORLD_SURFACE for every column by scanning down.
    ///
    /// What a freshly loaded or generated chunk needs once; maintaining it
    /// incrementally afterwards is what `set_block` does.
    void recompute_world_surface() noexcept;

    /// Non-air blocks across the whole column.
    [[nodiscard]] usize non_air_count() const noexcept;

private:
    [[nodiscard]] usize section_index_for_y(i32 y) const noexcept;
    [[nodiscard]] i32   scan_surface_down(usize x, usize z, i32 from_y) const noexcept;

    ChunkPos                  position_;
    WorldShape                shape_;
    AirStates                 air_;
    std::vector<ChunkSection> sections_;

    /// Indexed by HeightmapType for the four stored kinds.
    std::array<Heightmap, kStoredHeightmapCount> heightmaps_;
};

}  // namespace ov::world
