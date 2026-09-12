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
//
// ── Copy-on-write ───────────────────────────────────────────────────────────
//
// CLAUDE.md principle 3: the tick thread is the only writer of a published
// chunk, and what another thread sees goes through `shared_ptr<const>`. A
// section keeps its blocks, biomes and light in one shared storage, so copying
// a section — which is what `Chunk::snapshot` does 24 times — copies a pointer,
// not up to 12 KiB.
//
// The rule that makes this safe is deliberately blunt: **a storage two
// sections have ever seen is never written again.** Copying marks both sides
// shared; whichever side writes first takes its own storage and stops being
// shared. Nothing ever asks `use_count()` — a count read on the tick thread
// while the network thread drops its reference would need a standalone fence
// to be correct, and ThreadSanitizer does not model those. The cost is one
// copy of each section written after it was sent, which is the copy
// copy-on-write means anyway.
//
// A reference returned by a const accessor (`blocks()`, `sky_light()`) points
// into the current storage. Anything that writes a section should therefore
// get it from a writable `Chunk::section_for_y`, which unshares before handing
// it out: a reference taken afterwards stays valid until the section is copied
// again, and copies happen only when the tick sends a chunk.
#pragma once

#include "ov/registry/block_states.hpp"
#include "ov/world/light_array.hpp"
#include "ov/world/paletted_container.hpp"

#include <memory>

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

    /// Shares the storage and marks both sides: whichever writes first copies.
    ChunkSection(const ChunkSection& other) noexcept;
    ChunkSection& operator=(const ChunkSection& other) noexcept;

    ChunkSection(ChunkSection&&) noexcept            = default;
    ChunkSection& operator=(ChunkSection&&) noexcept = default;
    ~ChunkSection()                                  = default;

    [[nodiscard]] registry::BlockStateId get_block(usize x, usize y, usize z) const noexcept;

    /// Place a block, keeping the non-air count current.
    ///
    /// The count is maintained here rather than recomputed, because the chunk
    /// packet needs it on every send and walking 4096 entries per section per
    /// send would dominate the packet.
    void set_block(usize x, usize y, usize z, registry::BlockStateId state);

    [[nodiscard]] u16 get_biome(usize x, usize y, usize z) const noexcept;
    void              set_biome(usize x, usize y, usize z, u16 biome);

    /// Make every cell the same biome, and say so in the representation.
    void fill_biome(u16 biome);

    /// Non-air blocks. This is the wire's "block count" field verbatim.
    [[nodiscard]] u16 non_air_count() const noexcept { return storage_->non_air_count; }

    /// True when the section holds nothing but air.
    ///
    /// Vanilla skips these when meshing and still sends them, so this is a
    /// rendering and ticking decision rather than a wire one.
    [[nodiscard]] bool is_empty() const noexcept { return storage_->non_air_count == 0; }

    [[nodiscard]] const PalettedContainer& blocks() const noexcept { return storage_->blocks; }

    [[nodiscard]] const PalettedContainer& biomes() const noexcept { return storage_->biomes; }

    /// Writable light. Unshares first, so it may copy the section's storage.
    [[nodiscard]] LightArray& block_light() {
        unshare();
        return storage_->block_light;
    }

    [[nodiscard]] const LightArray& block_light() const noexcept { return storage_->block_light; }

    /// Writable light. Unshares first, so it may copy the section's storage.
    [[nodiscard]] LightArray& sky_light() {
        unshare();
        return storage_->sky_light;
    }

    [[nodiscard]] const LightArray& sky_light() const noexcept { return storage_->sky_light; }

    /// Replace the block storage wholesale, as reading a chunk does, and
    /// recount. Returns false if the packed data does not match its declared
    /// shape.
    [[nodiscard]] bool load_blocks(u8 bits, std::span<const u16> palette,
                                   std::span<const u64> data);

    [[nodiscard]] bool load_biomes(u8 bits, std::span<const u16> palette,
                                   std::span<const u64> data);

    /// Recompute the non-air count from scratch.
    void recount();

    /// Make this section the only owner of its storage, copying it if it has
    /// ever been shared. Every writer calls it; `Chunk::section_for_y` calls it
    /// before handing a section out.
    void unshare();

    /// True while the storage may be seen by another section. For tests and
    /// for the report that counts copies.
    [[nodiscard]] bool is_shared() const noexcept { return shared_; }

private:
    struct Storage {
        PalettedContainer blocks;
        PalettedContainer biomes;
        LightArray        block_light;
        LightArray        sky_light;
        u16               non_air_count{0};
    };

    AirStates                air_;
    std::shared_ptr<Storage> storage_;
    /// Mutable because copying a const section marks it too: that is what
    /// keeps the *original* from writing a storage its copy can read.
    mutable bool shared_{false};
};

}  // namespace ov::world
