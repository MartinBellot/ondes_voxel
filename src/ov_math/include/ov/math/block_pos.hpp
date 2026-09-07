// Positions in the three coordinate spaces the world uses.
//
//   BlockPos   — one block, world space
//   ChunkPos   — one 16x16 column, world space / 16
//   SectionPos — one 16x16x16 cube, the unit of storage, meshing and lighting
//
// Since 1.18 the overworld runs from y = -64 to y = 319: 384 blocks, 24
// sections, with section index -4 at the bottom. Every off-by-one in this file
// is a corrupted chunk on disk, so the conversions are floor division, never
// truncation towards zero.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

namespace ov {

/// Blocks along one edge of a chunk section.
inline constexpr i32 kSectionSize = 16;
/// Blocks in a section: 16^3.
inline constexpr i32 kSectionVolume = kSectionSize * kSectionSize * kSectionSize;
/// Biome cells in a section: 4x4x4, one per 4-block cube.
inline constexpr i32 kBiomeCellsPerSection = 64;

/// Floor division. `-1 / 16` is 0 in C++ but the block at y = -1 belongs to
/// section -1, not section 0. Truncation here silently writes blocks into the
/// wrong region file.
[[nodiscard]] constexpr i32 floor_div(i32 value, i32 divisor) noexcept {
    const i32 quotient = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

/// Non-negative remainder, matching floor_div.
[[nodiscard]] constexpr i32 floor_mod(i32 value, i32 divisor) noexcept {
    const i32 remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

struct ChunkPos;
struct SectionPos;

struct BlockPos {
    i32 x{};
    i32 y{};
    i32 z{};

    constexpr BlockPos() noexcept = default;
    constexpr BlockPos(i32 x_, i32 y_, i32 z_) noexcept : x{x_}, y{y_}, z{z_} {}
    constexpr explicit BlockPos(const Vec3i& v) noexcept : x{v.x}, y{v.y}, z{v.z} {}

    friend constexpr bool operator==(const BlockPos&, const BlockPos&) noexcept = default;

    [[nodiscard]] constexpr BlockPos offset(Direction d) const noexcept {
        const Vec3i o = direction_offset(d);
        return {x + o.x, y + o.y, z + o.z};
    }

    [[nodiscard]] constexpr BlockPos offset(i32 dx, i32 dy, i32 dz) const noexcept {
        return {x + dx, y + dy, z + dz};
    }

    [[nodiscard]] constexpr BlockPos above(i32 n = 1) const noexcept { return {x, y + n, z}; }
    [[nodiscard]] constexpr BlockPos below(i32 n = 1) const noexcept { return {x, y - n, z}; }

    /// Coordinates within the containing section, each in [0, 16).
    [[nodiscard]] constexpr i32 local_x() const noexcept { return floor_mod(x, kSectionSize); }
    [[nodiscard]] constexpr i32 local_y() const noexcept { return floor_mod(y, kSectionSize); }
    [[nodiscard]] constexpr i32 local_z() const noexcept { return floor_mod(z, kSectionSize); }

    /// Index into a section's 4096-entry block array.
    ///
    /// The order is Y, then Z, then X — so consecutive indices walk along X.
    /// This is the order the Anvil format and the network chunk packet both
    /// use; choosing a different one would mean transposing on every read and
    /// write of every chunk.
    [[nodiscard]] constexpr i32 section_index() const noexcept {
        return (local_y() * kSectionSize + local_z()) * kSectionSize + local_x();
    }

    [[nodiscard]] constexpr ChunkPos to_chunk() const noexcept;
    [[nodiscard]] constexpr SectionPos to_section() const noexcept;
};

/// A 16x16 column of the world, spanning the full build height.
struct ChunkPos {
    i32 x{};
    i32 z{};

    constexpr ChunkPos() noexcept = default;
    constexpr ChunkPos(i32 x_, i32 z_) noexcept : x{x_}, z{z_} {}

    friend constexpr bool operator==(const ChunkPos&, const ChunkPos&) noexcept = default;

    /// World-space block coordinate of the column's minimum corner.
    [[nodiscard]] constexpr i32 min_block_x() const noexcept { return x * kSectionSize; }
    [[nodiscard]] constexpr i32 min_block_z() const noexcept { return z * kSectionSize; }

    /// Which region file holds this chunk. Regions are 32x32 chunks.
    [[nodiscard]] constexpr i32 region_x() const noexcept { return floor_div(x, 32); }
    [[nodiscard]] constexpr i32 region_z() const noexcept { return floor_div(z, 32); }

    /// Slot within the region header's 1024-entry table, indexed z-major.
    [[nodiscard]] constexpr i32 region_slot() const noexcept {
        return floor_mod(z, 32) * 32 + floor_mod(x, 32);
    }

    /// Chebyshev distance in chunks — the metric view distance uses, since a
    /// render distance of 12 means a 25x25 square, not a circle.
    [[nodiscard]] constexpr i32 chebyshev_distance(const ChunkPos& o) const noexcept {
        const i32 dx = x > o.x ? x - o.x : o.x - x;
        const i32 dz = z > o.z ? z - o.z : o.z - z;
        return dx > dz ? dx : dz;
    }

    /// Packed into 64 bits for use as a hash-map key.
    [[nodiscard]] constexpr u64 packed() const noexcept {
        return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(z);
    }

    [[nodiscard]] static constexpr ChunkPos from_packed(u64 packed) noexcept {
        return {static_cast<i32>(static_cast<u32>(packed >> 32)),
                static_cast<i32>(static_cast<u32>(packed))};
    }
};

/// One 16x16x16 cube: the unit of block storage, meshing and light propagation.
struct SectionPos {
    i32 x{};
    i32 y{};
    i32 z{};

    constexpr SectionPos() noexcept = default;
    constexpr SectionPos(i32 x_, i32 y_, i32 z_) noexcept : x{x_}, y{y_}, z{z_} {}

    friend constexpr bool operator==(const SectionPos&, const SectionPos&) noexcept = default;

    [[nodiscard]] constexpr ChunkPos to_chunk() const noexcept { return {x, z}; }

    /// World-space minimum corner of the section.
    [[nodiscard]] constexpr BlockPos min_block() const noexcept {
        return {x * kSectionSize, y * kSectionSize, z * kSectionSize};
    }
};

constexpr ChunkPos BlockPos::to_chunk() const noexcept {
    return {floor_div(x, kSectionSize), floor_div(z, kSectionSize)};
}

constexpr SectionPos BlockPos::to_section() const noexcept {
    return {floor_div(x, kSectionSize), floor_div(y, kSectionSize), floor_div(z, kSectionSize)};
}

}  // namespace ov

template <>
struct std::hash<ov::ChunkPos> {
    [[nodiscard]] std::size_t operator()(const ov::ChunkPos& p) const noexcept {
        return std::hash<ov::u64>{}(p.packed());
    }
};
