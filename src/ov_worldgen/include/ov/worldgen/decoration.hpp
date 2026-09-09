// When each feature happens, and with what seed.
//
// This is the part that decides *where* everything lands, and it is entirely
// arithmetic — no noise, no terrain, nothing that could be eyeballed. Get it
// wrong and every ore, tree and spring is plausible and in the wrong place,
// with no symptom anywhere.
//
// Three mechanisms, in order:
//
//   1. The **decoration seed**. Per chunk: seed the generator from the world
//      seed, take two odd longs from it, and combine them with the chunk's
//      corner. That number is the chunk's, and it is what makes two worlds
//      with the same seed identical and two neighbouring chunks independent.
//
//   2. The **step**. `GenerationStep.Decoration` has eleven values in a fixed
//      order, and a biome's `features` array is eleven lists in that order.
//      Lakes are cut before the ores are placed; the ores before the springs;
//      the springs before the grass. A biome does not choose the order.
//
//   3. The **feature seed**. Within a step, features are numbered from zero and
//      each is seeded `decoration + index + 10000 * step`. The index is a
//      position in an ordering shared by *every* biome — not the biome's own
//      list — so that a chunk which straddles a forest and a desert places
//      their shared ores in one agreed order rather than twice.
//
// The shared ordering is what FeatureSorter builds: a topological sort over
// the constraint "in every biome, this feature comes before that one". Two
// features that never appear in the same biome are unordered by that
// constraint, and the tie is broken by the order the biomes were read in,
// which is why they are read in sorted order and not in whatever order the
// filesystem hands back.
#pragma once

#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/feature.hpp"

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// The eleven decoration steps, in the order the game runs them.
///
/// The numbering is not ours to choose: it multiplies into every feature seed
/// as `10000 * step`, so renumbering them moves every feature in the world.
enum class DecorationStep : u8 {
    RawGeneration        = 0,
    Lakes                = 1,
    LocalModifications   = 2,
    UndergroundStructures = 3,
    SurfaceStructures    = 4,
    Strongholds          = 5,
    UndergroundOres      = 6,
    UndergroundDecoration = 7,
    FluidSprings         = 8,
    VegetalDecoration    = 9,
    TopLayerModification = 10,
};

inline constexpr usize kDecorationStepCount = 11;

[[nodiscard]] std::string_view to_string(DecorationStep step) noexcept;

/// The seed one chunk's decoration starts from.
///
/// Two odd longs drawn from the world seed, one scaled by the chunk's x and one
/// by its z, exclusive-ored back against the world seed. Odd because an even
/// multiplier would lose a bit of the coordinate at every doubling and make
/// distant chunks repeat.
[[nodiscard]] i64 decoration_seed(i64 level_seed, i32 chunk_min_x, i32 chunk_min_z,
                                 FeatureRandom::Kind kind) noexcept;

/// The seed one feature gets, given the chunk's and where it sits.
[[nodiscard]] i64 feature_seed(i64 decoration, i32 index, i32 step) noexcept;

/// The features of every biome, ordered, and the machinery to run them.
class Decorator {
public:
    /// Read every biome's feature lists from `worldgen/biome/` and build the
    /// shared per-step ordering over them.
    [[nodiscard]] static std::expected<Decorator, FeatureError> load(
        const std::filesystem::path& data_root, const registry::BlockRegistry& blocks,
        const FeatureRegistry& features);

    /// Decorate one chunk.
    ///
    /// `level` must cover the chunk *and its eight neighbours*: a vein started
    /// in the last column of a chunk finishes in the next one, and the biomes
    /// that decide what is placed are collected from the whole neighbourhood.
    /// Returns how many features actually placed something.
    usize decorate(FeatureLevel& level, i32 chunk_x, i32 chunk_z, i64 level_seed) const;

    /// The names of the placed features at one step, in the order the sorter
    /// put them. Exposed because the order is the answer: a parity harness that
    /// wants to know why a vein moved needs to see the index, not deduce it.
    [[nodiscard]] std::vector<std::string_view> order_at(DecorationStep step) const;

    /// The index a placed feature has at its step, or -1 if it has none.
    [[nodiscard]] i32 index_of(DecorationStep step, std::string_view feature) const;

    /// Which biomes list a feature at all. The `biome` modifier's question.
    [[nodiscard]] const BiomeFeatures& biome_features() const noexcept;

    /// How many placed features named by a biome could not be built, and so
    /// will never happen. Not an error — the interpreter is incomplete on
    /// purpose — but a number that has to be visible.
    [[nodiscard]] usize missing_count() const noexcept;
    [[nodiscard]] std::vector<std::string> missing() const;

    [[nodiscard]] usize biome_count() const noexcept;

    Decorator(Decorator&&) noexcept;
    Decorator& operator=(Decorator&&) noexcept;
    ~Decorator();

private:
    struct Impl;

    Decorator();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
