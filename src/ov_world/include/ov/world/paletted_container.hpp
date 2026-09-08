// The bit-packed storage a chunk section uses for blocks and for biomes.
//
// This one container backs two formats that must both be exact: the Anvil
// section on disk and the chunk packet on the wire. An error here breaks saving
// and networking at the same time, which is why the layout is spelled out
// rather than inferred.
//
// The packing rule, and the one people get wrong: entries are packed into
// 64-bit longs at a fixed width, and **an entry never spans two longs**. With
// 5-bit entries a long holds 12 and wastes 4 bits. That is the post-1.16 rule;
// before then entries did span, and reading a modern chunk with the old rule
// produces plausible garbage rather than an error.
//
// Three representations, chosen by how many distinct values the section holds:
//
//   single value   one id, no data at all. The common case by a wide margin —
//                  most sections are entirely air or entirely stone.
//   indirect       a palette, with indices packed at 4 to 8 bits for blocks and
//                  1 to 3 for biomes. The two ranges are different and the
//                  width is what a reader uses to tell the encodings apart.
//   direct         no palette, ids packed at the registry's own width. 15 bits
//                  for 24135 block states, 6 for the biomes.
//
// The width is a property of the container, not of each entry, so changing one
// block can force the whole section to be repacked. That is expensive and it is
// why the width only ever grows.
#pragma once

#include "ov/base/types.hpp"

#include <span>
#include <vector>

namespace ov::world {

/// How a container currently stores its entries.
enum class PaletteKind : u8 {
    /// Every entry is the same value. No packed data is stored at all.
    SingleValue,
    /// Indices into a small palette: 4 to 8 bits for blocks, 1 to 3 for biomes.
    Indirect,
    /// Registry ids packed directly, no palette.
    Direct,
};

/// Blocks: below this, vanilla still pads the width up to 4 bits.
inline constexpr u8 kMinIndirectBits = 4;

/// Blocks: above this, a palette costs more than it saves and the container
/// goes direct.
inline constexpr u8 kMaxIndirectBits = 8;

/// Biomes use a *different* range, and this is not a detail.
///
/// A biome palette is 1 to 3 bits; four or more means direct. So padding a
/// biome palette up to 4 bits the way blocks are padded does not merely waste
/// space — the client reads the width to decide which encoding it is looking
/// at, sees 4, and interprets palette indices as registry ids. Every biome in
/// the chunk becomes a different one.
inline constexpr u8 kMinBiomeIndirectBits = 1;
inline constexpr u8 kMaxBiomeIndirectBits = 3;

/// Width of a directly packed block state: 24135 states need 15 bits.
inline constexpr u8 kDirectBlockBits = 15;

/// Width of a directly packed biome: 1.20.1 has 64, so 6 bits.
inline constexpr u8 kDirectBiomeBits = 6;

/// How many entries fit in one long at a given width.
///
/// Integer division, deliberately: the remainder is padding, because an entry
/// may not span two longs.
[[nodiscard]] constexpr u32 entries_per_long(u8 bits) noexcept {
    return bits == 0 ? 0 : 64u / bits;
}

/// Longs needed to hold `count` entries at `bits` each.
[[nodiscard]] constexpr usize packed_length(usize count, u8 bits) noexcept {
    if (bits == 0) {
        return 0;
    }
    const u32 per_long = entries_per_long(bits);
    return (count + per_long - 1) / per_long;
}

/// The smallest palette width that can index `size` distinct values, before
/// the per-kind minimum is applied.
[[nodiscard]] constexpr u8 raw_bits_for_palette(usize size) noexcept {
    u8 bits = 1;
    while ((usize{1} << bits) < size) {
        ++bits;
    }
    return bits;
}

/// The width a *block* palette uses for `size` entries.
[[nodiscard]] constexpr u8 bits_for_palette(usize size) noexcept {
    const u8 bits = raw_bits_for_palette(size);
    return bits < kMinIndirectBits ? kMinIndirectBits : bits;
}

/// The width a *biome* palette uses for `size` entries.
[[nodiscard]] constexpr u8 bits_for_biome_palette(usize size) noexcept {
    const u8 bits = raw_bits_for_palette(size);
    return bits < kMinBiomeIndirectBits ? kMinBiomeIndirectBits : bits;
}

/// A section's worth of entries: 4096 blocks, or 64 biomes.
///
/// Values are u16 registry ids. The container knows nothing about what they
/// mean, which is what lets blocks and biomes share it.
class PalettedContainer {
public:
    /// A container of `capacity` entries, all set to `value`.
    ///
    /// `min_indirect_bits` and `max_indirect_bits` differ between blocks and
    /// biomes, and the width is what tells a reader which encoding it is
    /// looking at — so they are part of the container's identity rather than a
    /// detail of the writer.
    PalettedContainer(usize capacity, u16 value, u8 direct_bits, u8 min_indirect_bits,
                      u8 max_indirect_bits);

    /// A block section: 4096 entries of air.
    [[nodiscard]] static PalettedContainer blocks(u16 fill = 0);

    /// A biome section: 4x4x4 cells.
    [[nodiscard]] static PalettedContainer biomes(u16 fill = 0);

    [[nodiscard]] usize capacity() const noexcept { return capacity_; }

    [[nodiscard]] PaletteKind kind() const noexcept { return kind_; }

    [[nodiscard]] u8 bits() const noexcept { return bits_; }

    /// The palette, empty when direct and one entry when single-valued.
    [[nodiscard]] std::span<const u16> palette() const noexcept { return palette_; }

    /// The packed longs, exactly as they appear on disk and on the wire.
    [[nodiscard]] std::span<const u64> data() const noexcept { return data_; }

    [[nodiscard]] u16 get(usize index) const noexcept;

    /// Set one entry, growing the representation when it no longer fits.
    ///
    /// Growing repacks every entry, so this is not free. It is also monotonic:
    /// the width never shrinks on its own, because a section that held 40
    /// distinct blocks once will very likely do so again, and shrinking then
    /// growing repeatedly is worse than staying wide.
    void set(usize index, u16 value);

    /// Replace the contents wholesale, choosing the tightest representation.
    /// This is what reading a section from disk or from the network does.
    void assign(std::span<const u16> values);

    /// Distinct values currently representable. Not a count of what is in use.
    [[nodiscard]] usize palette_size() const noexcept;

    /// True when every entry is the same value.
    [[nodiscard]] bool is_single_valued() const noexcept {
        return kind_ == PaletteKind::SingleValue;
    }

    /// Rebuild from packed data that came from disk or the wire.
    ///
    /// Returns false when the data does not match the declared width and
    /// palette — a chunk that lies about its own shape has to be rejected
    /// rather than read past.
    [[nodiscard]] bool load_packed(u8 bits, std::span<const u16> palette,
                                   std::span<const u64> data);

private:
    void repack(u8 new_bits, std::span<const u16> new_palette);

    [[nodiscard]] u8 bits_for(usize palette_size) const noexcept;

    usize            capacity_{0};
    u8               direct_bits_{kDirectBlockBits};
    u8               min_indirect_bits_{kMinIndirectBits};
    u8               max_indirect_bits_{kMaxIndirectBits};
    u8               bits_{0};
    PaletteKind      kind_{PaletteKind::SingleValue};
    std::vector<u16> palette_;
    std::vector<u64> data_;
};

}  // namespace ov::world
