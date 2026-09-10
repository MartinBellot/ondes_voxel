// The carvers: caves and canyons, cut out of the terrain the noise made.
//
// A carver is almost entirely random numbers. There is no noise field to get
// approximately right and no interpolation to smooth a small error away: a
// tunnel is a chain of ellipsoids whose every centre, radius and turn comes off
// one generator, in one order. Draw once too often, or once too few times, and
// the caves are still caves — the right number of them, the right size, the
// right depth — and not one of them is where the game put it. That failure
// looks like success in every measure except the only one that counts.
//
// So this file is written around the draw sequence rather than around the
// geometry, and it is checked against the game's own answer rather than against
// a screenshot: a chunk the game generated but has not yet finished still
// carries `CarvingMasks.AIR`, the exact set of cells its carvers cut. That is a
// bit-for-bit oracle, and `tools/ov_carveparity` compares against it.
//
// Two things in here are traps that a reasonable implementation walks into:
//
//   * the angles do not come from libm. The game reads them out of a 65536-entry
//     float table, and the table's answer differs from `sinf`'s in the last
//     places. Over a hundred tunnel steps that difference compounds into tens of
//     blocks;
//   * C++ does not sequence the operands of `a - b`. Java does. Every expression
//     here that draws more than one random number per line has been split into
//     named locals, because `next_float() - next_float()` is allowed to evaluate
//     right to left and would silently swap two draws.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/carving_mask.hpp"

#include <string_view>

namespace ov::worldgen {

/// The dimension's vertical extent, which is all a carver needs to know about
/// the world.
///
/// Named after the game's `CarvingContext` and deliberately as small as it is:
/// the set of cells a carver cuts depends on the seed, the chunk and these two
/// numbers, and on nothing else. Not on the terrain, not on the biome, not on
/// the aquifer. That is why a mask can be compared against the game's without
/// having any of the rest working, and it is the strongest testing lever in
/// this module.
struct CarvingContext {
    i32 min_y{-64};
    /// The chunk's height, which sizes the mask.
    i32 height{384};

    // ── nether ──
    /// How far above the bottom the carvers turn a cell into lava rather than
    /// air. Eight in the overworld (`lava_level: {above_bottom: 8}`); 31 in the
    /// Nether, whose carver ignores its own `lava_level` field and uses a
    /// fixed 31 — measured, see docs/provenance/nether.md.
    i32 lava_offset{8};
    /// The generator's depth — the noise settings' `height` — when it differs
    /// from the chunk's. The Nether's chunks are 256 tall and its noise 128,
    /// and `below_top` and the carvers' top margin count from the second.
    /// Zero means "the chunk height".
    i32 gen_depth{0};

    [[nodiscard]] constexpr i32 depth() const noexcept {
        return gen_depth > 0 ? gen_depth : height;
    }

    /// The lowest level the carvers may turn into lava rather than air.
    [[nodiscard]] constexpr i32 lava_level() const noexcept { return min_y + lava_offset; }
};

// ── The game's trigonometry ─────────────────────────────────────────────────

/// `Mth.sin`: a lookup into a 65536-entry table of floats, not `std::sin`.
///
/// The index is `(int)(value * 10430.378f) & 0xFFFF`, so the function is a step
/// function with 65536 steps and its answer is wrong by up to 5e-5 relative to
/// the real sine. Using the real one is the single easiest way to build caves
/// that are the right shape and in the wrong place.
[[nodiscard]] f32 mth_sin(f32 value) noexcept;

/// `Mth.cos`, which is the same table offset by a quarter turn.
[[nodiscard]] f32 mth_cos(f32 value) noexcept;

// ── Seeding ─────────────────────────────────────────────────────────────────

/// `WorldgenRandom.setLargeFeatureSeed`, the per-chunk seeding every carver and
/// every structure start goes through.
///
/// Seed the generator from the base seed, take two longs from it, combine them
/// with the chunk coordinates, and seed again. Both draws matter: skipping
/// either one gives a perfectly good per-chunk seed that shares nothing with
/// the game's.
[[nodiscard]] math::LegacyRandomSource large_feature_random(i64 seed, i32 chunk_x,
                                                            i32 chunk_z) noexcept;

// ── The carvers ─────────────────────────────────────────────────────────────

/// One configured carver, exactly as `data/.../worldgen/configured_carver`
/// spells it out.
///
/// The values are not defaults invented here: they are read off the data
/// generator's own JSON, and the comment on each says which field it came from.
/// Only the fields that change between the three overworld carvers are stored;
/// what all three share is written into the code, where it can carry the
/// explanation it needs.
struct CaveCarverConfig {
    /// `probability` — how often a chunk starts a cave at all.
    f32 probability{0.15F};
    /// `y.min_inclusive`, resolved against the dimension.
    i32 y_min{-56};
    /// `y.max_inclusive`, resolved against the dimension.
    i32 y_max{180};
    /// ── nether ── `minecraft:nether_cave`: `NetherWorldCarver`, which is the
    /// cave carver with four things changed — at most 10 rather than 15 in the
    /// outer draw, a thicker tunnel (`(f * 2 + f) * 2`, never the one-in-ten
    /// widening), a vertical-to-horizontal ratio of 5, and every shape
    /// provider in its JSON a *constant*, so the three multipliers and the
    /// room's y-scale draw nothing.
    bool nether{false};
};

/// `minecraft:cave`: the common one, anywhere from just above bedrock to y=180.
[[nodiscard]] CaveCarverConfig cave_config(const CarvingContext& context) noexcept;

/// `minecraft:cave_extra_underground`: the same carver again, rarer and capped
/// at y=47, which is what makes the deep layers denser than the shallow ones.
[[nodiscard]] CaveCarverConfig cave_extra_underground_config(
    const CarvingContext& context) noexcept;

/// `minecraft:nether_cave`: probability 0.2, y uniform from absolute 0 to
/// `below_top 1` of the generator's depth — 126 in a 128-deep Nether.
[[nodiscard]] CaveCarverConfig nether_cave_config(const CarvingContext& context) noexcept;

/// The cave carver of 1.18 and later.
///
/// A start chunk draws a handful of origins; each origin optionally opens with
/// a room and then sends one or more tunnels out of it. A tunnel walks up to
/// ~112 steps, carving an ellipsoid at most of them, wandering as two damped
/// drift terms push its yaw and pitch around, and may fork once into two
/// branches at a step chosen in advance.
class CaveWorldCarver {
public:
    CaveWorldCarver(CarvingContext context, CaveCarverConfig config)
        : context_(context), config_(config) {}

    /// Whether this chunk starts a cave. Consumes one float either way.
    [[nodiscard]] bool is_start_chunk(math::LegacyRandomSource& random) const noexcept;

    /// Carve everything a start at (`origin_x`, `origin_z`) puts inside the
    /// chunk (`chunk_x`, `chunk_z`). The two are usually different chunks:
    /// tunnels reach far enough that a chunk is carved by its neighbours far
    /// more often than by itself.
    void carve(math::LegacyRandomSource& random, i32 origin_x, i32 origin_z, i32 chunk_x,
               i32 chunk_z, CarvingMask& mask) const;

private:
    CarvingContext   context_;
    CaveCarverConfig config_;
};

/// `minecraft:canyon`: the ravines.
///
/// One start, one shaft, no branches — but its width varies per layer, from a
/// table of factors drawn once for the whole world column, which is what gives
/// a ravine its ragged silhouette instead of a smooth tube.
class CanyonWorldCarver {
public:
    explicit CanyonWorldCarver(CarvingContext context) : context_(context) {}

    [[nodiscard]] bool is_start_chunk(math::LegacyRandomSource& random) const noexcept;

    void carve(math::LegacyRandomSource& random, i32 origin_x, i32 origin_z, i32 chunk_x,
               i32 chunk_z, CarvingMask& mask) const;

private:
    CarvingContext context_;
};

// ── The stage ───────────────────────────────────────────────────────────────

/// The `air` carving step for one chunk.
///
/// Every overworld biome in 1.20.1 lists exactly the same three carvers in
/// exactly the same order — `cave`, `cave_extra_underground`, `canyon` — which
/// was checked across all 53 of them rather than assumed. That is what lets
/// this run without a biome lookup: the biome would change the list in the
/// nether, and the nether is not this.
///
/// The loop is the part that is easy to get subtly wrong. A chunk is carved by
/// every chunk within 8 in each direction, itself included: 17 x 17 x 3 = 867
/// seedings per chunk, each replaying its own chunk's carvers and keeping only
/// what lands here.
/// Which dimension's carver list a stage runs.
///
/// ── nether ── Every Nether biome lists exactly one carver, `nether_cave`, at
/// index 0 — checked across all five, as the overworld's three were across 53.
enum class CarverPreset : u8 {
    /// `cave`, `cave_extra_underground`, `canyon`; cut cells become `air`.
    Overworld,
    /// `nether_cave`; cut cells become `cave_air`, lava at and below y = 31.
    Nether,
};

class CarverStage {
public:
    CarverStage(i64 seed, CarvingContext context, CarverPreset preset = CarverPreset::Overworld);

    /// The Nether's stage for a world seed: min y 0, 256-block chunks, a
    /// 128-deep generator, lava at 31.
    [[nodiscard]] static CarverStage nether(i64 seed);

    [[nodiscard]] const CarvingContext& context() const noexcept { return context_; }
    [[nodiscard]] CarverPreset          preset() const noexcept { return preset_; }

    /// The block tag that says what these carvers may cut —
    /// `#minecraft:overworld_carver_replaceables` or
    /// `#minecraft:nether_carver_replaceables`.
    [[nodiscard]] std::string_view replaceables_tag() const noexcept;

    /// What a cut cell above the lava level becomes: `minecraft:air` for the
    /// overworld's carvers, `minecraft:cave_air` for the Nether's.
    [[nodiscard]] std::string_view carved_air() const noexcept;

    /// The mask for one chunk. Depends on nothing but the seed, the chunk and
    /// the world's height.
    [[nodiscard]] CarvingMask carve(i32 chunk_x, i32 chunk_z) const;

    /// The same, into a mask the caller already owns — so that generating many
    /// chunks does not allocate 12 KB per chunk. The mask is cleared first.
    void carve_into(i32 chunk_x, i32 chunk_z, CarvingMask& mask) const;

private:
    i64               seed_;
    CarvingContext    context_;
    CarverPreset      preset_;
    CaveWorldCarver   cave_;
    CaveWorldCarver   cave_extra_;
    CanyonWorldCarver canyon_;
};

}  // namespace ov::worldgen
