// One 16x16x16 cube of the world: its blocks, its biomes and its light.
//
// The unit everything else is expressed in. Anvil stores a chunk as a list of
// these, the chunk packet sends a list of these, the mesher meshes one of
// these, and the light engine propagates between them.
//
// Indices run in **YZX** order — `(y * 16 + z) * 16 + x` — because that is what
// the file format and the wire format use. Choosing XZY instead would work
// perfectly, right up until a chunk was written or a packet sent, and would
// then produce a world that is a transposition of itself.
#pragma once

#include "ov/registry/block_states.hpp"
#include "ov/world/light_array.hpp"
#include "ov/world/paletted_container.hpp"

namespace ov::world {

/// Blocks along one edge of a section.
inline constexpr usize kSectionSize = 16;

/// Biome cells along one edge: biomes are stored per 4x4x4 cube.
inline constexpr usize kBiomeSize = 4;

/// The three blocks that count as air.
///
/// The distinction matters for the wire: a section's "block count" field counts
/// non-air blocks, and a client told a cave is solid renders it solid. Resolved
/// from the registry rather than written down — these are Mojang's ids, and the
/// project's rule is that we never author one.
struct AirStates {
    registry::BlockStateId air{0};
    registry::BlockStateId void_air{0};
    registry::BlockStateId cave_air{0};

    /// Look the three up. Falls back to state 0 alone if the registry is
    /// missing them, which only happens with a pack from another version.
    [[nodiscard]] static AirStates from(const registry::BlockRegistry& blocks);

    [[nodiscard]] bool is_air(registry::BlockStateId state) const noexcept {
        return state == air || state == void_air || state == cave_air;
    }
};

/// Index of a block within a section, in the order the formats use.
[[nodiscard]] constexpr usize section_index(usize x, usize y, usize z) noexcept {
    return (y * kSectionSize + z) * kSectionSize + x;
}

/// Index of a biome cell: the same order over the 4x4x4 grid.
[[nodiscard]] constexpr usize biome_index(usize x, usize y, usize z) noexcept {
    return ((y / 4) * kBiomeSize + (z / 4)) * kBiomeSize + (x / 4);
}

class ChunkSection {
public:
    /// A section filled with air, with no light stored.
    explicit ChunkSection(AirStates air);

    [[nodiscard]] registry::BlockStateId get_block(usize x, usize y, usize z) const noexcept;

    /// Place a block, keeping the non-air count current.
    ///
    /// The count is maintained here rather than recomputed, because the chunk
    /// packet needs it on every send and walking 4096 entries per section per
    /// send would dominate the packet.
    void set_block(usize x, usize y, usize z, registry::BlockStateId state);

    [[nodiscard]] u16 get_biome(usize x, usize y, usize z) const noexcept;
    void              set_biome(usize x, usize y, usize z, u16 biome);

    /// Non-air blocks. This is the wire's "block count" field verbatim.
    [[nodiscard]] u16 non_air_count() const noexcept { return non_air_count_; }

    /// True when the section holds nothing but air.
    ///
    /// Vanilla skips these when meshing and still sends them, so this is a
    /// rendering and ticking decision rather than a wire one.
    [[nodiscard]] bool is_empty() const noexcept { return non_air_count_ == 0; }

    [[nodiscard]] const PalettedContainer& blocks() const noexcept { return blocks_; }

    [[nodiscard]] const PalettedContainer& biomes() const noexcept { return biomes_; }

    [[nodiscard]] LightArray& block_light() noexcept { return block_light_; }

    [[nodiscard]] const LightArray& block_light() const noexcept { return block_light_; }

    [[nodiscard]] LightArray& sky_light() noexcept { return sky_light_; }

    [[nodiscard]] const LightArray& sky_light() const noexcept { return sky_light_; }

    /// Replace the block storage wholesale, as reading a chunk does, and
    /// recount. Returns false if the packed data does not match its declared
    /// shape.
    [[nodiscard]] bool load_blocks(u8 bits, std::span<const u16> palette,
                                   std::span<const u64> data);

    [[nodiscard]] bool load_biomes(u8 bits, std::span<const u16> palette,
                                   std::span<const u64> data);

    /// Recompute the non-air count from scratch.
    void recount() noexcept;

private:
    AirStates         air_;
    PalettedContainer blocks_;
    PalettedContainer biomes_;
    LightArray        block_light_;
    LightArray        sky_light_;
    u16               non_air_count_{0};
};

}  // namespace ov::world
