// The End's generation: the island noise, the fixed biome rule, the spikes.
//
// The End is the one dimension of 1.20.1 whose shape is not a table read out of
// the datapack. Its `noise_settings` name a density node, `minecraft:end_islands`,
// that has no parameters at all — the whole algorithm is the node — and its
// biome source is not multi-noise: `TheEndBiomeSource` is a rule, four
// thresholds on that same node read at the centre of each chunk. So both live
// here, as code, and both are checked cell by cell against an End the real
// server generated (`scripts/reference_end.sh`, `tools/ov_endparity`,
// docs/provenance/end.md).
//
// What the node is, as the technical documentation describes it:
//
//   * a 2D simplex noise seeded from `new LegacyRandomSource(seed)` after 17292
//     draws have been thrown away (66 x 262: the octaves the pre-1.18 End
//     generator created before its island noise);
//   * a height at every eighth block: 100 - 8 x distance to the origin, and
//     wherever the simplex noise dips below -0.9 on a grid of every other
//     block of that eighth-scale plane more than 64 away from the origin, a
//     small island whose steepness is a hash of its own coordinates;
//   * the density is `(height - 8) / 128`.
//
// Every float in it is a Java float and every division a truncating one; the
// comparison against -0.9 is against the float -0.9F widened. Written from the
// description, confirmed by measurement, not read from anyone's code.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/density.hpp"

#include <array>
#include <string_view>

namespace ov::worldgen {

/// Two-dimensional simplex noise over a 256-entry permutation.
///
/// Seeded exactly like one octave of `ImprovedNoise` — three offsets drawn and
/// then the same partial Fisher-Yates — although the two-dimensional sample
/// does not use the offsets. They are drawn because drawing them is part of
/// the seed.
class SimplexNoise {
public:
    explicit SimplexNoise(math::LegacyRandomSource& random);

    [[nodiscard]] f64 value(f64 x, f64 y) const noexcept;

private:
    [[nodiscard]] i32 permute(i32 value) const noexcept {
        return permutation_[static_cast<usize>(value & 255)];
    }

    std::array<i32, 256> permutation_{};
    f64                  xo_{0.0};
    f64                  yo_{0.0};
    f64                  zo_{0.0};
};

/// The `end_islands` density: the main island and the outer ones.
class EndIslands {
public:
    /// How many draws the seeding throws away before the simplex noise.
    static constexpr i32 kDiscardedDraws = 17292;

    explicit EndIslands(i64 seed);

    /// The height field at one point of the eighth-scale plane — block
    /// coordinates divided by eight, truncating. In [-100, 80].
    [[nodiscard]] f32 height_value(i32 x, i32 z) const noexcept;

    /// The density node's value at a block: `(height - 8) / 128`.
    [[nodiscard]] f64 density(i32 block_x, i32 block_z) const noexcept;

    static constexpr f64 kMinValue = -0.84375;
    static constexpr f64 kMaxValue = 0.5625;

private:
    SimplexNoise noise_;
};

/// The density node, for the router. One per router: the node keeps a small
/// memo keyed by the eighth-scale column (the value depends on nothing else),
/// which makes it as thread-hostile as the rest of a router — one stack per
/// thread, as everywhere in worldgen.
[[nodiscard]] DensityRef make_end_islands_density(i64 seed);

// ── The biome rule ──────────────────────────────────────────────────────────

/// The End's five biomes, in the order its biome source yields them. The
/// order is load-bearing: the feature sorter breaks its ties by it.
inline constexpr std::array<std::string_view, 5> kEndBiomes{
    "minecraft:the_end", "minecraft:end_highlands", "minecraft:end_midlands",
    "minecraft:small_end_islands", "minecraft:end_barrens"};

/// Beyond this squared chunk distance from the origin the islands' own biomes
/// begin: 64 chunks, 1024 blocks.
inline constexpr i64 kEndCentreChunksSquared = 4096;

/// Which of `kEndBiomes` a value of the router's `erosion` (the island
/// density at the chunk's centre) means, outside the central disc.
[[nodiscard]] usize end_biome_for_erosion(f64 erosion) noexcept;

/// The rule, whole: the biome index at a *quart* position. Reads the router's
/// `erosion` entry at the block at the centre of the quart's chunk, y = 0 —
/// the same answer for every cell of a chunk, and for every y.
[[nodiscard]] usize end_biome_at(const NoiseRouter& router, i32 quart_x, i32 quart_z);

// ── The spikes ──────────────────────────────────────────────────────────────

/// One obsidian pillar of the main island.
struct EndSpike {
    i32  centre_x{0};
    i32  centre_z{0};
    i32  radius{0};
    i32  height{0};
    /// An iron-bar cage round the crystal: the two pillars whose index in the
    /// shuffle is 1 or 2.
    bool guarded{false};
};

/// The ten spikes of a world. A pure function of the seed: ten positions on a
/// circle of radius 42, and a shuffle of 0..9 — seeded by the low sixteen bits
/// of the first long of `new Random(seed)` — that gives each its radius
/// (2 + i / 3), height (76 + 3 i) and cage.
[[nodiscard]] std::array<EndSpike, 10> end_spikes(i64 level_seed);

}  // namespace ov::worldgen
