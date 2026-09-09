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
    i32 height{384};

    /// The lowest level the carvers may turn into lava rather than air —
    /// `lava_level: {above_bottom: 8}` in every overworld carver.
    [[nodiscard]] constexpr i32 lava_level() const noexcept { return min_y + 8; }
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
};

/// `minecraft:cave`: the common one, anywhere from just above bedrock to y=180.
[[nodiscard]] CaveCarverConfig cave_config(const CarvingContext& context) noexcept;

/// `minecraft:cave_extra_underground`: the same carver again, rarer and capped
/// at y=47, which is what makes the deep layers denser than the shallow ones.
[[nodiscard]] CaveCarverConfig cave_extra_underground_config(
    const CarvingContext& context) noexcept;

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
class CarverStage {
public:
    CarverStage(i64 seed, CarvingContext context);

    [[nodiscard]] const CarvingContext& context() const noexcept { return context_; }

    /// The mask for one chunk. Depends on nothing but the seed, the chunk and
    /// the world's height.
    [[nodiscard]] CarvingMask carve(i32 chunk_x, i32 chunk_z) const;

    /// The same, into a mask the caller already owns — so that generating many
    /// chunks does not allocate 12 KB per chunk. The mask is cleared first.
    void carve_into(i32 chunk_x, i32 chunk_z, CarvingMask& mask) const;

private:
    i64               seed_;
    CarvingContext    context_;
    CaveWorldCarver   cave_;
    CaveWorldCarver   cave_extra_;
    CanyonWorldCarver canyon_;
};

}  // namespace ov::worldgen
