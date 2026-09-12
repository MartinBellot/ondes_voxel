// ── worldgen-3 ── The fuzzy zoom between a block and its biome cell.
//
// Biomes are stored per 4x4x4 cell, but the game never reads the cell that
// contains a block: `BiomeManager` jitters the lookup by up to about half a
// cell, so that biome borders are not straight lines of four. Every biome
// question asked of a *world* goes through it — the `biome` placement filter,
// the surface rules, the freezing of the top layer — while the biome source's
// own answers (which biome a cell *is*) do not.
//
// The jitter is seeded by the world seed passed through SHA-256
// (`BiomeManager.obfuscateSeed`): the first eight bytes of the digest of the
// seed's eight little-endian bytes, read little-endian. Each of the eight cells
// around the block gets a pseudo-random offset from a linear congruential mix
// of its coordinates and that seed; the nearest offset cell wins.
//
// Documented in docs/provenance/features.md, « Le zoom flou des biomes ».
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <span>

namespace ov::worldgen {

/// SHA-256 of a message. Exposed so it can be checked against the standard's
/// own test vectors.
[[nodiscard]] std::array<u8, 32> sha256(std::span<const u8> message) noexcept;

/// `BiomeManager.obfuscateSeed(seed)`.
[[nodiscard]] i64 obfuscate_biome_seed(i64 world_seed) noexcept;

/// A biome cell, in quart coordinates (block >> 2).
struct BiomeCell {
    i32 x{0};
    i32 y{0};
    i32 z{0};

    friend constexpr bool operator==(const BiomeCell&, const BiomeCell&) noexcept = default;
};

/// The cell whose biome the game reads for the block `(x, y, z)`, given the
/// obfuscated seed. Always the block's own cell or one of its neighbours
/// towards the block's nearest corner.
[[nodiscard]] BiomeCell fuzzy_biome_cell(i64 zoom_seed, i32 x, i32 y, i32 z) noexcept;

}  // namespace ov::worldgen
