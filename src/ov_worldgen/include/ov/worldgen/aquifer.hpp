// The aquifer: what fills a block that the terrain left empty.
//
// Without it every cave below the sea level is flooded and every cave above it
// is dry, which is the "global fluid rule" and nothing more. With it the world
// is cut into cells, each cell gets a fluid level and a fluid of its own, and a
// block that sits between two cells whose fluids disagree may be turned back
// into stone — the barrier that keeps an underground lake from pouring into the
// dry cave next to it.
//
// Where each part of this file comes from, and it matters because the parts do
// not come from the same place (docs/provenance/aquiferes.md § 10):
//
//   * **documented** — the Minecraft Wiki's `Cave` article, section `Aquifer`,
//     and jacobsjo's aquifer explanation: the 16 x 12 x 16 grid and its shift,
//     the random centre per cell, the four nearest centres and the threshold of
//     25 on their squared distances, the thirteen surface samples raised by
//     eight, the flooding thresholds and their linear blend over 64 blocks, the
//     spread and lava samples on their 16 x 40 x 16 and 64 x 40 x 64 grids, the
//     exclusion region, and the qualitative shape of the pressure;
//   * **measured** — every choice the documents leave open (the direction of
//     the grid shift, the order of the three draws, where the thirteen surfaces
//     are sampled, the constants of the pressure), each settled against the
//     game's own blocks with `ov_parity --aquifer`, and each with a witness
//     that the measurement would have rejected a wrong answer.
//
// The documents describe *behaviour*; no source code was read for this file.
//
// Two traps, both of the "looks right, is wrong" kind:
//
//   * the object is **per chunk and per thread**. It memoises every cell status
//     and every preliminary surface it computes, and the router beneath it has
//     mutable caches of its own. Build one per chunk; never share one.
//   * the carvers ask the aquifer too, with a density of exactly zero. A carved
//     cell below an aquifer's level fills with that aquifer's fluid, and a
//     carved cell on a barrier is not carved at all.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/density.hpp"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::worldgen {

/// The fluid a status holds below its level.
enum class AquiferFluid : u8 { None, Water, Lava };

/// What an aquifer cell holds: a fluid up to (not including) a level.
///
/// A dry cell is a status whose level is below the world, so no block of the
/// world is ever under it.
struct FluidStatus {
    i32          level{0};
    AquiferFluid fluid{AquiferFluid::None};

    /// The fluid at height `y`, or none above the level.
    [[nodiscard]] constexpr AquiferFluid at(i32 y) const noexcept {
        return y < level ? fluid : AquiferFluid::None;
    }

    friend constexpr bool operator==(const FluidStatus&, const FluidStatus&) = default;
};

/// What the aquifer decided for one block.
enum class Substance : u8 {
    /// Stone: the terrain was solid, or a barrier kept it.
    Solid,
    Air,
    Water,
    Lava,
};

/// The answer for one block, and whether the fluid placed there has to be woken
/// up once the chunk exists — the waterfalls and lavafalls between two cells.
struct AquiferAnswer {
    Substance substance{Substance::Air};
    bool      schedule{false};
};

/// Every choice the documents leave open, with the answer the measurement gave
/// as the default.
///
/// Exposed so a harness can put a different answer in and watch the agreement
/// fall — which is the witness each of these defaults rests on. Generation
/// never changes them.
struct AquiferTuning {
    /// The grid shift, as "the block's cell is `floor((x + shift_x) / 16)`".
    /// The documents say "shifted by 5 on X and Z and 1 on Y" and not in
    /// which direction.
    i32 shift_xz{-5};
    i32 shift_y{1};

    /// The order the three offsets are drawn in: 0 = x, y, z. Any other value
    /// is a witness order (1 = z, y, x; 2 = y, x, z).
    i32 draw_order{0};

    /// The name the aquifer's positional factory is hashed from. A witness
    /// changes it and the whole grid of centres moves.
    std::string random_name{"minecraft:aquifer"};

    /// Where the thirteen surfaces are sampled. True: the centre moved by
    /// sixteen blocks per chunk offset. False: the origin of the chunk the
    /// offset lands in.
    bool surface_from_centre{true};

    /// Step of the downward search for the preliminary surface, in blocks.
    i32 surface_step{8};

    /// Whether a partially flooded level is capped at the lowest *raised*
    /// surface (true) or at the lowest raw one (false).
    bool cap_raised{false};

    // ── The pressure between two statuses ───────────────────────────────────
    //
    // Documented in shape only: positive between the two levels and highest in
    // the middle, a lid of at most about 5 blocks above the higher level, walls
    // of at most about 23 blocks below the lower one, a strong fixed pressure
    // between water and lava, a barrier noise in a narrow band. The "5" and
    // "23" of the article are `band × above_out` and `below_bias + band ×
    // below_out`, and that is where these values come from.
    //
    // What the carved-cell oracle then said about each (aquiferes.md § 10.3):
    // `below_bias`, `below_in`, `below_out` and `above_in` sit on an interior
    // maximum; `above_out` does not — a steeper slope keeps improving to the
    // edge of the sweep, so it stays at the article's value rather than being
    // fitted to that edge; `gain`, `chain_similarity`, `water_lava` and `band`
    // cannot be seen at a density of zero at all and rest on the article
    // alone.

    /// Divisor of the distance inside the band, upper half.
    f64 above_in{1.5};
    /// Divisor of the distance outside, above the higher level.
    f64 above_out{2.5};
    /// Offset added below the middle before dividing: the floor starts this
    /// many blocks below the lower level.
    f64 below_bias{3.0};
    /// Divisor inside, lower half.
    f64 below_in{3.0};
    /// Divisor outside, below the lower level.
    f64 below_out{10.0};
    /// The barrier noise is only read where the raw pressure is within this.
    f64 band{2.0};
    /// The pressure between water and lava, whatever the levels.
    f64 water_lava{2.0};
    /// The factor the pressure is multiplied by before it meets the density.
    f64 gain{2.0};
    /// Whether the third centre's pairs carry the first pair's similarity as a
    /// factor (true) or only their own (false).
    bool chain_similarity{true};
    /// Whether the barrier noise is read at all.
    bool use_barrier_noise{true};

    /// A fluid is woken once the chunk exists when the second centre is
    /// closer than this to the first, in squared distance — **whatever the
    /// two statuses are**. Measured against the game's `PostProcessing`
    /// marks: every gap from 0 to 44 is marked, 45 and over is not, and the
    /// statuses make no difference, which is not what the article's "cells
    /// with identical statuses are never scheduled" says
    /// (docs/provenance/aquiferes.md § 10.4).
    i32 schedule_gap{45};
};

/// The seed-level half: the noises, the factory, the constants. Built once per
/// generator stack, immutable afterwards.
class Aquifer {
public:
    /// Build from a router. `enabled()` is false, and the reason is in
    /// `missing()`, if the router lacks any of the entries the aquifer reads.
    Aquifer(const NoiseRouter& router, i64 seed, AquiferTuning tuning = {});

    /// Whether every router entry this needs was built.
    [[nodiscard]] bool enabled() const noexcept { return missing_.empty(); }

    /// The router entries that are missing, by name. Empty when enabled.
    [[nodiscard]] const std::vector<std::string>& missing() const noexcept { return missing_; }

    [[nodiscard]] const AquiferTuning& tuning() const noexcept { return tuning_; }

    /// The rule every dimension has, aquifers or not: lava below
    /// `min(-54, sea level)`, the default fluid up to the sea level.
    [[nodiscard]] FluidStatus global_fluid(i32 y) const noexcept;

    [[nodiscard]] i32 sea_level() const noexcept { return sea_level_; }
    [[nodiscard]] i32 min_y() const noexcept { return min_y_; }

    /// The level of a dry cell: one below the world.
    [[nodiscard]] i32 dry_level() const noexcept { return min_y_ - 1; }

    /// The cell centre of grid cell (`gx`, `gy`, `gz`), in blocks.
    [[nodiscard]] std::array<i32, 3> centre(i32 gx, i32 gy, i32 gz) const noexcept;

    /// The grid cell a block's search starts from, per axis.
    [[nodiscard]] i32 grid_xz(i32 v) const noexcept;
    [[nodiscard]] i32 grid_y(i32 v) const noexcept;

private:
    friend class AquiferSampler;

    AquiferTuning          tuning_;
    math::XoroshiroPositionalFactory positional_{0, 0};

    const DensityFunction* barrier_{nullptr};
    const DensityFunction* floodedness_{nullptr};
    const DensityFunction* spread_{nullptr};
    const DensityFunction* lava_{nullptr};
    const DensityFunction* erosion_{nullptr};
    const DensityFunction* depth_{nullptr};
    const DensityFunction* surface_density_{nullptr};

    i32 sea_level_{63};
    i32 min_y_{-64};
    i32 height_{384};

    std::vector<std::string> missing_;
};

/// The four nearest centres of a block, nearest first, and their statuses.
/// What `compute` decides from; exposed so a harness can test a rule other
/// than the one `compute` applies against the game's own answer.
struct AquiferNeighbourhood {
    std::array<i32, 4>         distance{};
    std::array<FluidStatus, 4> status{};
};

/// The per-chunk half: memoised statuses and surfaces.
///
/// Cheap to build and meant to be thrown away with the chunk. Not thread-safe
/// and not meant to be: see the header.
class AquiferSampler {
public:
    explicit AquiferSampler(const Aquifer& aquifer) : aquifer_(&aquifer) {}

    /// What fills (`x`, `y`, `z`) given the terrain's density there. The noise
    /// stage passes `final_density`; the carvers pass zero.
    [[nodiscard]] AquiferAnswer compute(i32 x, i32 y, i32 z, f64 density);

    /// The status of the centre of a grid cell. Exposed for the harness.
    [[nodiscard]] FluidStatus status_of_cell(i32 gx, i32 gy, i32 gz);

    /// The four nearest centres of a block and their statuses. For harnesses.
    [[nodiscard]] AquiferNeighbourhood neighbourhood(i32 x, i32 y, i32 z);

    /// The preliminary surface of a column: the highest sampled height where
    /// `initial_density_without_jaggedness` exceeds 0.390625.
    [[nodiscard]] i32 preliminary_surface(i32 x, i32 z);

private:
    /// The four nearest centres among the twelve cells around a block, as
    /// squared distances and grid cells, nearest first.
    void nearest(i32 x, i32 y, i32 z, std::array<i32, 4>& distance,
                 std::array<std::array<i32, 3>, 4>& cell);

    [[nodiscard]] FluidStatus compute_status(i32 x, i32 y, i32 z);
    [[nodiscard]] i32         surface_level(i32 x, i32 y, i32 z, FluidStatus global,
                                            i32 lowest_surface, bool own_under_sea);
    [[nodiscard]] AquiferFluid fluid_type(i32 x, i32 y, i32 z, FluidStatus global, i32 level);
    [[nodiscard]] f64 pressure(i32 x, i32 y, i32 z, FluidStatus first, FluidStatus second);

    const Aquifer* aquifer_;

    /// The barrier noise at the block being answered, read at most once per
    /// block however many pairs ask for it.
    f64  barrier_value_{0.0};
    bool barrier_known_{false};

    std::unordered_map<i64, FluidStatus>        statuses_;
    std::unordered_map<i64, std::array<i32, 3>> centres_;
    std::unordered_map<i64, i32>                surfaces_;
};

[[nodiscard]] std::string_view to_string(Substance substance) noexcept;

}  // namespace ov::worldgen
