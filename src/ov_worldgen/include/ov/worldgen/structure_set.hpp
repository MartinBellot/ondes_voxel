// Where a structure may start — the arithmetic, and only the arithmetic.
//
// A structure set is a spacing, a separation, a salt and a rule. Together they
// answer one question, per chunk, with a boolean: *could* a structure of this
// set start here? Nothing about terrain, nothing about biomes, nothing about
// what the structure looks like. That separation is deliberate and it is what
// makes this file measurable: a chunk either is a placement chunk or it is not,
// and the game writes its answer into every chunk it saves
// (`structures.starts`), so the comparison is an exact boolean over thousands
// of chunks rather than a percentage over a noisy field.
//
// Two placement types exist in 1.20.1.
//
//   * `random_spread` divides the world into `spacing` x `spacing` cells and
//     puts one candidate chunk somewhere in the first `spacing - separation`
//     chunks of the cell, drawn from a generator seeded by the cell and the
//     salt. `linear` draws the offset once, `triangular` averages two draws,
//     which pulls candidates towards the middle of the cell and is why ocean
//     monuments and woodland mansions feel evenly spread rather than clumped.
//
//   * `concentric_rings` is the strongholds' own: 128 of them on rings around
//     the origin, and nothing else uses it.
//
// On top of the grid sits an optional *frequency reduction*: a second roll that
// throws most candidates away. Mineshafts are the extreme case — every chunk is
// a candidate (`spacing` 1) and 0.4 % survive. There are four reducers and they
// are not interchangeable; which one a set uses is in its JSON, and the mapping
// from the JSON name to the arithmetic was **measured**, not assumed. See
// docs/provenance/structures.md § 2.
//
// The seeding helpers are `WorldgenRandom`'s two, and they are different
// functions with confusingly similar names:
//
//   * `large_feature_seed(seed, x, z)` runs the world seed through
//     `java.util.Random` twice to get two multipliers, then folds the chunk in.
//     Used by the mineshaft reducer and by the weighted choice inside a set.
//   * `large_feature_with_salt(seed, x, z, salt)` is a plain affine combination
//     of the chunk, the seed and the salt. Used by the grid.
//
// Swapping them produces a perfectly plausible world that shares no structure
// with the real one.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

/// How the offset inside a grid cell is drawn.
enum class SpreadType : u8 {
    /// One `nextInt(range)`. Uniform over the cell.
    Linear = 0,
    /// The mean of two `nextInt(range)`. Peaked at the middle of the cell.
    Triangular = 1,
};

/// The second roll that throws candidates away.
///
/// The four are the game's, and the names in the JSON are opaque on purpose —
/// they are version numbers of a historical accident, not descriptions. What
/// each one does was pinned against the reference worlds; the mineshaft, whose
/// set is nothing *but* a reducer, decided `LegacyType3` on its own with 23
/// starts out of 5 790 chunks and no disagreement inside the force-loaded
/// patches.
enum class FrequencyReduction : u8 {
    /// `nextFloat() < frequency`, seeded with the set's own salt.
    Default = 0,
    /// The pillager outpost's: a hand-rolled seed, one discarded `nextInt()`,
    /// then `nextInt(1 / frequency) == 0`.
    LegacyType1 = 1,
    /// The buried treasure's: seeded with the *fixed* salt 10387320 — not the
    /// set's, which is 0 — then `nextFloat() < frequency`.
    LegacyType2 = 2,
    /// The mineshaft's: `large_feature_seed`, then `nextDouble() < frequency`.
    LegacyType3 = 3,
};

[[nodiscard]] std::string_view to_string(SpreadType type) noexcept;
[[nodiscard]] std::string_view to_string(FrequencyReduction reduction) noexcept;

/// `WorldgenRandom.setLargeFeatureSeed`. Two draws from the world seed become
/// the chunk's multipliers.
[[nodiscard]] i64 large_feature_seed(i64 level_seed, i32 chunk_x, i32 chunk_z) noexcept;

/// `WorldgenRandom.setLargeFeatureWithSalt`. Affine, no intermediate draws.
[[nodiscard]] i64 large_feature_with_salt(i64 level_seed, i32 chunk_x, i32 chunk_z,
                                          i32 salt) noexcept;

/// A set that keeps a candidate away from another set's candidates.
///
/// Only the pillager outposts have one: they refuse to start within ten chunks
/// of a village. Stored rather than resolved because the other set has to be
/// looked up by name, and a set cannot hold a pointer into the container it
/// lives in.
struct ExclusionZone {
    std::string other_set;
    i32         chunk_count{0};
};

/// `minecraft:random_spread`.
struct RandomSpreadPlacement {
    i32                spacing{1};
    i32                separation{0};
    SpreadType         spread{SpreadType::Linear};
    i32                salt{0};
    f32                frequency{1.0F};
    FrequencyReduction reduction{FrequencyReduction::Default};

    std::optional<ExclusionZone> exclusion;

    /// Where `/locate` points, relative to the start chunk's corner. Only the
    /// buried treasures set one (9, 0, 9). It changes no block; it is here so
    /// that a harness comparing against `/locate` output is comparing the same
    /// number.
    std::array<i32, 3> locate_offset{0, 0, 0};

    /// The one candidate chunk of the grid cell `(grid_x, grid_z)`.
    [[nodiscard]] ChunkPos candidate(i64 level_seed, i32 grid_x, i32 grid_z) const noexcept;

    /// Whether this chunk is the candidate of the cell it falls in.
    ///
    /// The grid step alone: the frequency roll and the exclusion zone are not
    /// applied here, because a caller measuring the grid wants the grid.
    [[nodiscard]] bool is_candidate_chunk(i64 level_seed, i32 chunk_x, i32 chunk_z) const noexcept;

    /// The frequency roll. True when the candidate survives.
    ///
    /// Always true when `frequency >= 1`, which is every set but four.
    [[nodiscard]] bool passes_frequency(i64 level_seed, i32 chunk_x, i32 chunk_z) const noexcept;
};

/// `minecraft:concentric_rings`. The strongholds, and only them.
///
/// The ring positions are a property of the *world*, not of a chunk: the game
/// computes all 128 of them once, at the first query, walking outward and
/// snapping each to the nearest chunk whose biome is in `preferred_biomes`.
/// That biome search is why this cannot be a pure function like the spread
/// placement, and why `StructureSetRegistry` does not answer it — it is stored
/// here and refused rather than approximated. See docs/provenance/structures.md
/// § 6.
struct ConcentricRingsPlacement {
    i32         count{128};
    i32         distance{32};
    i32         spread{3};
    i32         salt{0};
    std::string preferred_biomes;
};

/// One entry of a set: a structure and how likely it is to be the one chosen.
struct StructureSetEntry {
    std::string structure;
    i32         weight{1};
};

/// A structure set as `worldgen/structure_set/*.json` gives it.
struct StructureSet {
    std::string name;

    /// Exactly one of the two is engaged. `concentric` being present is how a
    /// caller learns that this set's placement is not arithmetic.
    std::optional<RandomSpreadPlacement>    spread;
    std::optional<ConcentricRingsPlacement> concentric;

    std::vector<StructureSetEntry> entries;

    /// The sum of the weights, for the weighted choice.
    [[nodiscard]] i32 total_weight() const noexcept;

    /// Which structure of the set is tried first in this chunk.
    ///
    /// A set with one entry always answers that one without drawing, which
    /// matters: drawing anyway would consume a number the game did not.
    [[nodiscard]] std::string_view choose(i64 level_seed, i32 chunk_x, i32 chunk_z) const;
};

enum class StructureSetError : u8 {
    /// `worldgen/structure_set/` is not there.
    Missing,
    /// A file is not JSON, or is JSON of the wrong shape.
    Malformed,
    /// A placement type we do not implement. Named rather than defaulted: a set
    /// silently treated as `random_spread` would place structures with real
    /// confidence in the wrong places.
    UnknownPlacement,
    /// A spread type or a frequency reduction method we do not implement.
    UnknownSpread,
};

[[nodiscard]] std::string_view to_string(StructureSetError error) noexcept;

/// Every structure set of the pack, read once.
class StructureSetRegistry {
public:
    /// Read `worldgen/structure_set/` under the generated data root.
    ///
    /// `data_root` is the `data/minecraft` directory, the same one the
    /// decorator takes.
    [[nodiscard]] static std::expected<StructureSetRegistry, StructureSetError> load(
        const std::filesystem::path& data_root);

    [[nodiscard]] const std::vector<StructureSet>& sets() const noexcept { return sets_; }

    /// A set by its full name, `minecraft:villages`.
    [[nodiscard]] const StructureSet* find(std::string_view name) const noexcept;

    /// The set a structure belongs to, or null. A structure belongs to exactly
    /// one set in 1.20.1 and the loader refuses a pack where it does not.
    [[nodiscard]] const StructureSet* set_of(std::string_view structure) const noexcept;

private:
    std::vector<StructureSet> sets_;
};

}  // namespace ov::worldgen
