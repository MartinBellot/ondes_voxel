#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/carver.hpp"

#include "ov/base/assert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ov::worldgen {

namespace {

/// The game's float pi. `(float)Math.PI`, not the double narrowed later: the
/// tunnel angle is computed entirely in float and the two differ.
constexpr f32 kPiF = 3.14159265358979323846F;

/// How far a carver reaches, in chunks, and the same number in blocks.
///
/// `WorldCarver.getRange()` is 4; a tunnel is allowed `(range * 2 - 1) * 16`
/// steps, which is 112 blocks. The neighbourhood the driver sweeps is a
/// separate constant — 8 chunks — and is larger than 112 blocks would strictly
/// need, which is why it is written out rather than derived from the range.
constexpr i32 kCarverRange       = 4;
constexpr i32 kTunnelRangeBlocks = (kCarverRange * 2 - 1) * 16;
constexpr i32 kNeighbourhood     = 8;

/// `CaveWorldCarver.getCaveBound()`: the bound of the outermost of the three
/// nested draws that decide how many origins a start chunk gets.
constexpr i32 kCaveBound = 15;

/// The top of the world minus the eight blocks the carvers leave alone on a
/// chunk that is not being upgraded from an older format.
constexpr i32 kTopMargin = 7;

/// `Mth.floor(double)`.
[[nodiscard]] i32 mth_floor(f64 value) noexcept {
    return static_cast<i32>(std::floor(value));
}

/// `UniformFloat.sample`: one draw, scaled into `[min, max)`. Float throughout;
/// doing it in double changes the last bits and therefore the radius.
[[nodiscard]] f32 sample_uniform(math::LegacyRandomSource& random, f32 min, f32 max) noexcept {
    return random.next_float() * (max - min) + min;
}

/// `TrapezoidFloat.sample`: two draws, one over the ramp and one over the
/// plateau. The canyon's thickness is the only place the game uses it here.
[[nodiscard]] f32 sample_trapezoid(math::LegacyRandomSource& random, f32 min, f32 max,
                                   f32 plateau) noexcept {
    const f32 span  = max - min;
    const f32 ramp  = (span - plateau) / 2.0F;
    const f32 flat  = span - ramp;
    const f32 first = random.next_float();
    // Split across two locals: C++ leaves the evaluation order of `a * h + b *
    // g` unspecified, and swapping these two draws is a valid compilation.
    const f32 second = random.next_float();
    return min + first * flat + second * ramp;
}

/// `Mth.randomBetweenInclusive`, which is what a `uniform` height provider is.
[[nodiscard]] i32 sample_height(math::LegacyRandomSource& random, i32 min, i32 max) noexcept {
    return random.next_int(max - min + 1) + min;
}

/// The 65536-entry sine table the game reads its angles out of.
///
/// Built once, immutable, and shared: it is a constant, not state, so it does
/// not fall foul of the rule against global mutable singletons. 256 KB, which
/// is the price of the last four decimal places of every tunnel.
[[nodiscard]] const std::array<f32, 65536>& sin_table() noexcept {
    static const std::array<f32, 65536> table = [] {
        std::array<f32, 65536> values{};
        for (usize i = 0; i < values.size(); ++i) {
            values[i] =
                static_cast<f32>(std::sin(static_cast<f64>(i) * 3.141592653589793 * 2.0 / 65536.0));
        }
        return values;
    }();
    return table;
}

/// The table index, with Java's float-to-int conversion.
///
/// Java saturates a float that does not fit an int; C++ makes the conversion
/// undefined. The angles here stay far inside the range, but "stays inside the
/// range" is exactly the kind of assumption that holds until a drift term does
/// something unexpected, so the saturation is written out.
[[nodiscard]] usize sin_index(f32 scaled) noexcept {
    constexpr f32 kIntMax = 2147483648.0F;
    if (!(scaled > -kIntMax) || !(scaled < kIntMax)) {
        return scaled > 0.0F ? 0xFFFFU : 0U;
    }
    return static_cast<usize>(static_cast<u32>(static_cast<i32>(scaled)) & 0xFFFFU);
}

}  // namespace

f32 mth_sin(f32 value) noexcept {
    return sin_table()[sin_index(value * 10430.378F)];
}

f32 mth_cos(f32 value) noexcept {
    return sin_table()[sin_index(value * 10430.378F + 16384.0F)];
}

math::LegacyRandomSource large_feature_random(i64 seed, i32 chunk_x, i32 chunk_z) noexcept {
    math::LegacyRandomSource random{seed};
    const i64                a = random.next_long();
    const i64                b = random.next_long();
    // Signed 64-bit multiply that is allowed to overflow. Done through u64 so
    // the wrap is defined rather than left to the compiler's mood.
    const u64 mixed = (static_cast<u64>(static_cast<i64>(chunk_x)) * static_cast<u64>(a)) ^
                      (static_cast<u64>(static_cast<i64>(chunk_z)) * static_cast<u64>(b)) ^
                      static_cast<u64>(seed);
    random.set_seed(static_cast<i64>(mixed));
    return random;
}

CaveCarverConfig cave_config(const CarvingContext& context) noexcept {
    // configured_carver/cave.json: probability 0.15, y from above_bottom 8 to
    // absolute 180.
    return CaveCarverConfig{0.15F, context.min_y + 8, 180};
}

CaveCarverConfig cave_extra_underground_config(const CarvingContext& context) noexcept {
    // configured_carver/cave_extra_underground.json: probability 0.07, y from
    // above_bottom 8 to absolute 47.
    return CaveCarverConfig{0.07F, context.min_y + 8, 47};
}

// ── Shared geometry ─────────────────────────────────────────────────────────

namespace {

/// Whether a tunnel that is at (`x`, `z`) with `remaining` steps left could
/// still reach the chunk being carved.
///
/// A pure optimisation, and it has to be: it draws nothing, so cutting a tunnel
/// short here must not change the result. It cannot, because a step moves at
/// most one block and the test allows for every remaining step being spent
/// coming back.
[[nodiscard]] bool can_reach(i32 chunk_x, i32 chunk_z, f64 x, f64 z, i32 branch_index,
                             i32 branch_count, f32 thickness) noexcept {
    const f64 to_x      = x - static_cast<f64>(chunk_x * 16 + 8);
    const f64 to_z      = z - static_cast<f64>(chunk_z * 16 + 8);
    const f64 remaining = static_cast<f64>(branch_count - branch_index);
    const f64 reach     = static_cast<f64>(thickness + 2.0F + 16.0F);
    return to_x * to_x + to_z * to_z - remaining * remaining <= reach * reach;
}

/// Everything a skip test needs. The two carvers reject different cells inside
/// the same bounding ellipsoid — a cave has a flat floor, a ravine has a
/// per-layer width — so the shape is a parameter rather than two copies of the
/// loop.
struct SkipShape {
    /// A cave: reject anything at or below the floor, then the unit sphere.
    /// Null for a canyon.
    const f64* floor_level{nullptr};
    /// A canyon: the per-layer width factors, indexed by `y - min_y - 1`.
    /// Null for a cave.
    const std::vector<f32>* width_factors{nullptr};
    i32                     min_y{-64};

    [[nodiscard]] bool should_skip(f64 rel_x, f64 rel_y, f64 rel_z, i32 y) const noexcept {
        if (floor_level != nullptr) {
            if (rel_y <= *floor_level) {
                return true;
            }
            return rel_x * rel_x + rel_y * rel_y + rel_z * rel_z >= 1.0;
        }
        // The index is one below the layer, which is the game's own off-by-one
        // and is part of the shape: layer y is widened by the factor drawn for
        // the layer beneath it. The ellipsoid loop never reaches y = min_y + 1,
        // so the index cannot go negative, but it is clamped rather than
        // asserted because the cost is nothing and the alternative is a crash
        // on a world height nobody tried yet.
        const usize index = static_cast<usize>(std::max(y - min_y - 1, 0));
        const f32   width = index < width_factors->size() ? (*width_factors)[index] : 1.0F;
        return (rel_x * rel_x + rel_z * rel_z) * static_cast<f64>(width) + rel_y * rel_y / 6.0 >=
               1.0;
    }
};

/// Mark every cell of one ellipsoid that falls inside the chunk being carved.
///
/// The mask is set for a cell that passes the shape test whether or not the
/// block there could actually be replaced. That is the game's behaviour and it
/// is visible in a saved chunk: `CarvingMasks.AIR` records what the carvers
/// considered, and the block decision is taken separately afterwards.
void carve_ellipsoid(const CarvingContext& context, i32 chunk_x, i32 chunk_z, f64 x, f64 y, f64 z,
                     f64 horizontal_radius, f64 vertical_radius, const SkipShape& shape,
                     CarvingMask& mask) {
    const f64 middle_x = static_cast<f64>(chunk_x * 16 + 8);
    const f64 middle_z = static_cast<f64>(chunk_z * 16 + 8);
    const f64 limit    = 16.0 + horizontal_radius * 2.0;
    if (std::abs(x - middle_x) > limit || std::abs(z - middle_z) > limit) {
        return;
    }

    const i32 base_x = chunk_x * 16;
    const i32 base_z = chunk_z * 16;
    const i32 from_x = std::max(mth_floor(x - horizontal_radius) - base_x - 1, 0);
    const i32 to_x   = std::min(mth_floor(x + horizontal_radius) - base_x, 15);
    const i32 from_y = std::max(mth_floor(y - vertical_radius) - 1, context.min_y + 1);
    const i32 to_y   = std::min(mth_floor(y + vertical_radius) + 1,
                                context.min_y + context.height - 1 - kTopMargin);
    const i32 from_z = std::max(mth_floor(z - horizontal_radius) - base_z - 1, 0);
    const i32 to_z   = std::min(mth_floor(z + horizontal_radius) - base_z, 15);

    for (i32 local_x = from_x; local_x <= to_x; ++local_x) {
        const f64 rel_x = (static_cast<f64>(base_x + local_x) + 0.5 - x) / horizontal_radius;
        for (i32 local_z = from_z; local_z <= to_z; ++local_z) {
            const f64 rel_z = (static_cast<f64>(base_z + local_z) + 0.5 - z) / horizontal_radius;
            if (rel_x * rel_x + rel_z * rel_z >= 1.0) {
                continue;
            }
            // Top down, and strictly above `from_y`: the lowest row of the
            // bounding box is never carved.
            for (i32 y_at = to_y; y_at > from_y; --y_at) {
                const f64 rel_y = (static_cast<f64>(y_at) - 0.5 - y) / vertical_radius;
                if (shape.should_skip(rel_x, rel_y, rel_z, y_at)) {
                    continue;
                }
                mask.set(local_x, y_at, local_z);
            }
        }
    }
}

/// `CaveWorldCarver.getThickness`: two draws, and one in ten times a third and
/// a fourth that make a cave much wider than the rest.
[[nodiscard]] f32 cave_thickness(math::LegacyRandomSource& random) noexcept {
    const f32 first  = random.next_float();
    const f32 second = random.next_float();
    f32       result = first * 2.0F + second;
    if (random.next_int(10) == 0) {
        const f32 a = random.next_float();
        const f32 b = random.next_float();
        result *= a * b * 3.0F + 1.0F;
    }
    return result;
}

}  // namespace

// ── CaveWorldCarver ─────────────────────────────────────────────────────────

bool CaveWorldCarver::is_start_chunk(math::LegacyRandomSource& random) const noexcept {
    return random.next_float() <= config_.probability;
}

namespace {

/// One tunnel, and the two branches it may fork into.
///
/// Recursive because the game's is: a fork replaces the rest of the parent
/// tunnel with two children that continue from the same step index, so the
/// total length of a forked tunnel is bounded by the parent's `branch_count`
/// and the recursion is one level deep in practice.
void create_tunnel(const CarvingContext& context, i32 chunk_x, i32 chunk_z, i64 seed, f64 x, f64 y,
                   f64 z, f64 horizontal_multiplier, f64 vertical_multiplier, f32 thickness,
                   f32 yaw, f32 pitch, i32 branch_index, i32 branch_count,
                   f64 horizontal_vertical_ratio, const SkipShape& shape, CarvingMask& mask) {
    math::LegacyRandomSource random{seed};
    const i32                fork_at     = random.next_int(branch_count / 2) + branch_count / 4;
    const bool               steep       = random.next_int(6) == 0;
    f32                      yaw_drift   = 0.0F;
    f32                      pitch_drift = 0.0F;

    for (i32 step = branch_index; step < branch_count; ++step) {
        // The radius follows half a sine over the tunnel's length: thin at both
        // ends, widest in the middle.
        const f64 radius   = 1.5 + static_cast<f64>(mth_sin(kPiF * static_cast<f32>(step) /
                                                            static_cast<f32>(branch_count)) *
                                                    thickness);
        const f64 vertical = radius * horizontal_vertical_ratio;

        const f32 horizontal_step = mth_cos(pitch);
        x += static_cast<f64>(mth_cos(yaw) * horizontal_step);
        y += static_cast<f64>(mth_sin(pitch));
        z += static_cast<f64>(mth_sin(yaw) * horizontal_step);

        pitch *= steep ? 0.92F : 0.7F;
        pitch += pitch_drift * 0.1F;
        yaw += yaw_drift * 0.1F;
        pitch_drift *= 0.9F;
        yaw_drift *= 0.75F;
        {
            const f32 a = random.next_float();
            const f32 b = random.next_float();
            const f32 c = random.next_float();
            pitch_drift += (a - b) * c * 2.0F;
        }
        {
            const f32 a = random.next_float();
            const f32 b = random.next_float();
            const f32 c = random.next_float();
            yaw_drift += (a - b) * c * 4.0F;
        }

        if (step == fork_at && thickness > 1.0F) {
            // A quarter turn either way, a third of the pitch, and a fresh
            // thickness each. Four draws, in this order: the seed of the first
            // branch, its thickness, then the same for the second. The
            // branches themselves draw from their own generators and cannot
            // disturb this one.
            const i64 left_seed      = random.next_long();
            const f32 left_thickness = random.next_float() * 0.5F + 0.5F;
            create_tunnel(context, chunk_x, chunk_z, left_seed, x, y, z, horizontal_multiplier,
                          vertical_multiplier, left_thickness, yaw - kPiF / 2.0F, pitch / 3.0F,
                          step, branch_count, 1.0, shape, mask);
            const i64 right_seed      = random.next_long();
            const f32 right_thickness = random.next_float() * 0.5F + 0.5F;
            create_tunnel(context, chunk_x, chunk_z, right_seed, x, y, z, horizontal_multiplier,
                          vertical_multiplier, right_thickness, yaw + kPiF / 2.0F, pitch / 3.0F,
                          step, branch_count, 1.0, shape, mask);
            return;
        }

        // One step in four carves nothing, which is what makes a cave a chain
        // of overlapping bulges rather than a smooth pipe. The draw happens
        // whether or not the tunnel is anywhere near this chunk.
        if (random.next_int(4) == 0) {
            continue;
        }
        if (!can_reach(chunk_x, chunk_z, x, z, step, branch_count, thickness)) {
            return;
        }
        carve_ellipsoid(context, chunk_x, chunk_z, x, y, z, radius * horizontal_multiplier,
                        vertical * vertical_multiplier, shape, mask);
    }
}

}  // namespace

void CaveWorldCarver::carve(math::LegacyRandomSource& random, i32 origin_x, i32 origin_z,
                            i32 chunk_x, i32 chunk_z, CarvingMask& mask) const {
    // Three nested draws, so most start chunks get no origin at all and a few
    // get a whole cave system. Nesting them is the point: a flat
    // `next_int(15)` would give the same average and a completely different
    // distribution.
    const i32 origins = random.next_int(random.next_int(random.next_int(kCaveBound) + 1) + 1);

    for (i32 origin = 0; origin < origins; ++origin) {
        const f64 x = static_cast<f64>(origin_x * 16 + random.next_int(16));
        const f64 y = static_cast<f64>(sample_height(random, config_.y_min, config_.y_max));
        const f64 z = static_cast<f64>(origin_z * 16 + random.next_int(16));

        // horizontal_radius_multiplier, vertical_radius_multiplier,
        // floor_level: three uniform draws, in the order the JSON's fields are
        // read in.
        const f64 horizontal_multiplier = static_cast<f64>(sample_uniform(random, 0.7F, 1.4F));
        const f64 vertical_multiplier   = static_cast<f64>(sample_uniform(random, 0.8F, 1.3F));
        const f64 floor_level           = static_cast<f64>(sample_uniform(random, -1.0F, -0.4F));

        const SkipShape shape{&floor_level, nullptr, context_.min_y};

        i32 tunnels = 1;
        if (random.next_int(4) == 0) {
            // A room: one wide, squat ellipsoid at the origin, and one to three
            // extra tunnels leaving it.
            const f64 y_scale     = static_cast<f64>(sample_uniform(random, 0.1F, 0.9F));
            const f32 room_radius = 1.0F + random.next_float() * 6.0F;
            const f64 radius      = 1.5 + static_cast<f64>(mth_sin(kPiF / 2.0F) * room_radius);
            // The room is carved one block east of the origin. Not a rounding
            // artefact — the game adds the block deliberately.
            carve_ellipsoid(context_, chunk_x, chunk_z, x + 1.0, y, z, radius, radius * y_scale,
                            shape, mask);
            tunnels += random.next_int(4);
        }

        for (i32 tunnel = 0; tunnel < tunnels; ++tunnel) {
            const f32 yaw       = random.next_float() * (kPiF * 2.0F);
            const f32 pitch     = (random.next_float() - 0.5F) / 4.0F;
            const f32 thickness = cave_thickness(random);
            const i32 branches  = kTunnelRangeBlocks - random.next_int(kTunnelRangeBlocks / 4);
            const i64 seed      = random.next_long();
            create_tunnel(context_, chunk_x, chunk_z, seed, x, y, z, horizontal_multiplier,
                          vertical_multiplier, thickness, yaw, pitch, 0, branches, 1.0, shape,
                          mask);
        }
    }
}

// ── CanyonWorldCarver ───────────────────────────────────────────────────────

bool CanyonWorldCarver::is_start_chunk(math::LegacyRandomSource& random) const noexcept {
    // configured_carver/canyon.json: probability 0.01.
    return random.next_float() <= 0.01F;
}

namespace {

/// The per-layer width factors that give a ravine its silhouette.
///
/// One factor per layer of the world, redrawn whenever a `next_int(3)` comes up
/// zero and held otherwise, so the profile is a staircase of plateaus about
/// three blocks tall. Squared, because the skip test compares squared
/// distances.
///
/// The first layer never draws the `next_int`: the condition is `layer == 0 ||
/// next_int(...) == 0` and Java short-circuits. One extra draw here shifts every
/// layer below it.
[[nodiscard]] std::vector<f32> init_width_factors(math::LegacyRandomSource& random, i32 height) {
    std::vector<f32> factors(static_cast<usize>(height));
    f32              current = 1.0F;
    for (i32 layer = 0; layer < height; ++layer) {
        if (layer == 0 || random.next_int(3) == 0) {
            const f32 a = random.next_float();
            const f32 b = random.next_float();
            current     = 1.0F + a * b;
        }
        factors[static_cast<usize>(layer)] = current * current;
    }
    return factors;
}

/// `CanyonWorldCarver.updateVerticalRadius`.
///
/// `vertical_radius_center_factor` is 0 and `vertical_radius_default_factor` is
/// 1 in the overworld canyon, so the shape term contributes nothing and the
/// whole function is one random draw between 0.75 and 1. It is written out in
/// full anyway: the factors are configuration, and a datapack that changes them
/// would otherwise be silently ignored.
[[nodiscard]] f64 update_vertical_radius(math::LegacyRandomSource& random, f64 vertical_radius,
                                         i32 branch_count, i32 branch_index) noexcept {
    constexpr f32 kCenterFactor  = 0.0F;
    constexpr f32 kDefaultFactor = 1.0F;
    const f32     along =
        1.0F -
        std::abs(0.5F - static_cast<f32>(branch_index) / static_cast<f32>(branch_count)) * 2.0F;
    const f32 factor = kDefaultFactor + kCenterFactor * along;
    return static_cast<f64>(factor) * vertical_radius *
           static_cast<f64>(sample_uniform(random, 0.75F, 1.0F));
}

}  // namespace

void CanyonWorldCarver::carve(math::LegacyRandomSource& random, i32 origin_x, i32 origin_z,
                              i32 chunk_x, i32 chunk_z, CarvingMask& mask) const {
    const f64 x = static_cast<f64>(origin_x * 16 + random.next_int(16));
    // configured_carver/canyon.json: y uniform over absolute 10 .. 67.
    const f64 y   = static_cast<f64>(sample_height(random, 10, 67));
    const f64 z   = static_cast<f64>(origin_z * 16 + random.next_int(16));
    const f32 yaw = random.next_float() * (kPiF * 2.0F);
    // vertical_rotation: uniform over [-0.125, 0.125).
    const f32 pitch = sample_uniform(random, -0.125F, 0.125F);
    // yScale is the bare number 3.0, which is a constant provider and draws
    // nothing. Reading it as a uniform would consume a float here and move
    // every ravine in the world.
    constexpr f64 kYScale = 3.0;
    // shape.thickness: a trapezoid over [0, 6] with a plateau of 2.
    const f32 thickness = sample_trapezoid(random, 0.0F, 6.0F, 2.0F);
    // shape.distance_factor: uniform over [0.75, 1).
    const i32 branch_count = static_cast<i32>(static_cast<f32>(kTunnelRangeBlocks) *
                                              sample_uniform(random, 0.75F, 1.0F));
    const i64 seed         = random.next_long();

    math::LegacyRandomSource shaft{seed};
    const std::vector<f32>   width_factors = init_width_factors(shaft, context_.height);
    const SkipShape          shape{nullptr, &width_factors, context_.min_y};

    f64 at_x        = x;
    f64 at_y        = y;
    f64 at_z        = z;
    f32 at_yaw      = yaw;
    f32 at_pitch    = pitch;
    f32 yaw_drift   = 0.0F;
    f32 pitch_drift = 0.0F;

    for (i32 step = 0; step < branch_count; ++step) {
        f64 radius   = 1.5 + static_cast<f64>(mth_sin(kPiF * static_cast<f32>(step) /
                                                      static_cast<f32>(branch_count)) *
                                              thickness);
        f64 vertical = radius * kYScale;
        // shape.horizontal_radius_factor, then the vertical one: two draws per
        // step, before the walk.
        radius *= static_cast<f64>(sample_uniform(shaft, 0.75F, 1.0F));
        vertical = update_vertical_radius(shaft, vertical, branch_count, step);

        const f32 horizontal_step = mth_cos(at_pitch);
        const f32 rise            = mth_sin(at_pitch);
        at_x += static_cast<f64>(mth_cos(at_yaw) * horizontal_step);
        at_y += static_cast<f64>(rise);
        at_z += static_cast<f64>(mth_sin(at_yaw) * horizontal_step);

        // A ravine turns half as sharply as a cave: 0.05 against 0.1, and the
        // drifts decay faster.
        at_pitch *= 0.7F;
        at_pitch += pitch_drift * 0.05F;
        at_yaw += yaw_drift * 0.05F;
        pitch_drift *= 0.8F;
        yaw_drift *= 0.5F;
        {
            const f32 a = shaft.next_float();
            const f32 b = shaft.next_float();
            const f32 c = shaft.next_float();
            pitch_drift += (a - b) * c * 2.0F;
        }
        {
            const f32 a = shaft.next_float();
            const f32 b = shaft.next_float();
            const f32 c = shaft.next_float();
            yaw_drift += (a - b) * c * 4.0F;
        }

        if (shaft.next_int(4) != 0) {
            if (!can_reach(chunk_x, chunk_z, at_x, at_z, step, branch_count, thickness)) {
                return;
            }
            carve_ellipsoid(context_, chunk_x, chunk_z, at_x, at_y, at_z, radius, vertical, shape,
                            mask);
        }
    }
}

// ── CarverStage ─────────────────────────────────────────────────────────────

CarverStage::CarverStage(i64 seed, CarvingContext context)
    : seed_(seed),
      context_(context),
      cave_(context, cave_config(context)),
      cave_extra_(context, cave_extra_underground_config(context)),
      canyon_(context) {}

CarvingMask CarverStage::carve(i32 chunk_x, i32 chunk_z) const {
    CarvingMask mask{context_.min_y, context_.height};
    carve_into(chunk_x, chunk_z, mask);
    return mask;
}

void CarverStage::carve_into(i32 chunk_x, i32 chunk_z, CarvingMask& mask) const {
    OV_ASSERT(mask.min_y() == context_.min_y && mask.height() == context_.height);
    mask.clear();

    for (i32 offset_x = -kNeighbourhood; offset_x <= kNeighbourhood; ++offset_x) {
        for (i32 offset_z = -kNeighbourhood; offset_z <= kNeighbourhood; ++offset_z) {
            const i32 origin_x = chunk_x + offset_x;
            const i32 origin_z = chunk_z + offset_z;

            // The index added to the seed is the carver's position in the
            // biome's list, and every overworld biome lists the same three in
            // the same order. Adding to the seed rather than mixing it in is
            // the game's choice and matters: seeds one apart share a carver
            // stream with each other, which is visible if you generate two of
            // them.
            {
                auto random = large_feature_random(seed_ + 0, origin_x, origin_z);
                if (cave_.is_start_chunk(random)) {
                    cave_.carve(random, origin_x, origin_z, chunk_x, chunk_z, mask);
                }
            }
            {
                auto random = large_feature_random(seed_ + 1, origin_x, origin_z);
                if (cave_extra_.is_start_chunk(random)) {
                    cave_extra_.carve(random, origin_x, origin_z, chunk_x, chunk_z, mask);
                }
            }
            {
                auto random = large_feature_random(seed_ + 2, origin_x, origin_z);
                if (canyon_.is_start_chunk(random)) {
                    canyon_.carve(random, origin_x, origin_z, chunk_x, chunk_z, mask);
                }
            }
        }
    }
}

}  // namespace ov::worldgen
