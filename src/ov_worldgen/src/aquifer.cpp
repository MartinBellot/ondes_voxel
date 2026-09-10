#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/aquifer.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ov::worldgen {

namespace {

/// Below this the global rule gives lava rather than the default fluid. The
/// article: "below Y = min(−54, sea level): lava, whose surface is at Y=-54".
constexpr i32 kLavaSurface = -54;

/// The threshold `initial_density_without_jaggedness` must exceed for a height
/// to count as the preliminary surface (jaggedness explanation, "Surface
/// height").
constexpr f64 kSurfaceThreshold = 0.390625;

/// The preliminary surface lies about eight blocks under the real one, and
/// every comparison raises it by that much (article and explanation agree).
constexpr i32 kSurfaceRaise = 8;

/// Half the height of a cell: the article's "bottom of the cell (centre Y −
/// 12)" and "top of the cell (centre Y + 12)".
constexpr i32 kCellReach = 12;

/// The squared-distance gap past which the nearest centre simply wins.
constexpr f64 kSimilarityRange = 25.0;

/// No density above the threshold anywhere in the column.
constexpr i32 kNoSurface = std::numeric_limits<i32>::max();

/// The thirteen chunk offsets the surface is sampled at, x then z. The
/// explanation lists them; the article describes the same shape ("3 chunks
/// west, 1 chunk east, and 1 chunk north and south"). The own chunk first,
/// because it is the one two of the rules single out.
constexpr std::array<std::array<i32, 2>, 13> kSurfaceOffsets{{
    {0, 0},
    {-2, -1},
    {-1, -1},
    {0, -1},
    {1, -1},
    {-3, 0},
    {-2, 0},
    {-1, 0},
    {1, 0},
    {-2, 1},
    {-1, 1},
    {0, 1},
    {1, 1},
}};

/// The exclusion region: the deep dark's. Floats in the explanation, and kept
/// as the doubles those floats widen to.
constexpr f64 kExclusionErosion = -0.22499999403953552;
constexpr f64 kExclusionDepth   = 0.8999999761581421;

[[nodiscard]] constexpr i32 floor_div(i32 value, i32 divisor) noexcept {
    const i32 quotient = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

/// Three coordinates into one key, each in its own bits. Not an XOR of shifted
/// values: that is how this module once had two distinct cells answer for
/// each other (docs/PROVENANCE.md, "La grille de cellules").
[[nodiscard]] constexpr i64 pack(i32 x, i32 y, i32 z) noexcept {
    const u64 ux = static_cast<u64>(static_cast<u32>(x)) & 0x3FFFFFFULL;
    const u64 uz = static_cast<u64>(static_cast<u32>(z)) & 0x3FFFFFFULL;
    const u64 uy = static_cast<u64>(static_cast<u32>(y)) & 0xFFFULL;
    return static_cast<i64>((ux << 38U) | (uz << 12U) | uy);
}

[[nodiscard]] constexpr f64 lerp(f64 t, f64 from, f64 to) noexcept {
    return from + t * (to - from);
}

/// How alike two squared distances are: 1 when equal, 0 at a gap of 25.
[[nodiscard]] constexpr f64 similarity(i32 nearer, i32 farther) noexcept {
    return 1.0 - static_cast<f64>(farther - nearer) / kSimilarityRange;
}

[[nodiscard]] constexpr Substance substance_of(AquiferFluid fluid) noexcept {
    switch (fluid) {
        case AquiferFluid::Water:
            return Substance::Water;
        case AquiferFluid::Lava:
            return Substance::Lava;
        case AquiferFluid::None:
            break;
    }
    return Substance::Air;
}

}  // namespace

std::string_view to_string(Substance substance) noexcept {
    switch (substance) {
        case Substance::Solid:
            return "solid";
        case Substance::Air:
            return "air";
        case Substance::Water:
            return "water";
        case Substance::Lava:
            return "lava";
    }
    return "unknown";
}

Aquifer::Aquifer(const NoiseRouter& router, i64 seed, AquiferTuning tuning)
    : tuning_(std::move(tuning)) {
    // The world seed's positional factory — the same root every noise is
    // seeded from — hashed by name and forked once more. "Chosen from the
    // world seed and the cell coordinates", the article says; which name the
    // hash is taken of is the measured part, and a wrong one moves every
    // centre (`AquiferTuning::random_name`).
    math::XoroshiroRandomSource source{seed};
    const math::XoroshiroPositionalFactory root = source.fork_positional();
    math::XoroshiroRandomSource            named = root.from_hash_of(tuning_.random_name);
    positional_                                  = named.fork_positional();

    sea_level_ = router.sea_level();
    min_y_     = router.min_y();
    height_    = router.height();

    const auto need = [&](std::string_view name) -> const DensityFunction* {
        const DensityFunction* function = router.entry(name);
        if (function == nullptr) {
            missing_.emplace_back(name);
        }
        return function;
    };
    barrier_         = need("barrier");
    floodedness_     = need("fluid_level_floodedness");
    spread_          = need("fluid_level_spread");
    lava_            = need("lava");
    erosion_         = need("erosion");
    depth_           = need("depth");
    surface_density_ = need("initial_density_without_jaggedness");

    if (!missing_.empty()) {
        OV_LOG_ERROR("worldgen: the aquifer is disabled, the router lacks {} of its entries "
                     "(first: {}); every cave will follow the global fluid rule",
                     missing_.size(), missing_.front());
    }
}

FluidStatus Aquifer::global_fluid(i32 y) const noexcept {
    if (y < std::min(kLavaSurface, sea_level_)) {
        return {kLavaSurface, AquiferFluid::Lava};
    }
    return {sea_level_, AquiferFluid::Water};
}

i32 Aquifer::grid_xz(i32 v) const noexcept {
    return floor_div(v + tuning_.shift_xz, 16);
}

i32 Aquifer::grid_y(i32 v) const noexcept {
    return floor_div(v + tuning_.shift_y, 12);
}

std::array<i32, 3> Aquifer::centre(i32 gx, i32 gy, i32 gz) const noexcept {
    // Three draws, each in its own statement: C++ does not sequence the
    // operands of an expression and the order of the draws is the point.
    math::XoroshiroRandomSource random = positional_.at(gx, gy, gz);
    i32                         ox     = 0;
    i32                         oy     = 0;
    i32                         oz     = 0;
    switch (tuning_.draw_order) {
        case 1:
            oz = random.next_int(10);
            oy = random.next_int(9);
            ox = random.next_int(10);
            break;
        case 2:
            oy = random.next_int(9);
            ox = random.next_int(10);
            oz = random.next_int(10);
            break;
        default:
            ox = random.next_int(10);
            oy = random.next_int(9);
            oz = random.next_int(10);
            break;
    }
    return {gx * 16 + ox, gy * 12 + oy, gz * 16 + oz};
}

// ── The per-chunk half ─────────────────────────────────────────────────────

i32 AquiferSampler::preliminary_surface(i32 x, i32 z) {
    const i64 key = pack(x, 0, z);
    if (const auto found = surfaces_.find(key); found != surfaces_.end()) {
        return found->second;
    }
    const Aquifer& aquifer = *aquifer_;
    const i32      step    = std::max(1, aquifer.tuning_.surface_step);
    i32            surface = kNoSurface;
    for (i32 y = aquifer.min_y_ + aquifer.height_; y >= aquifer.min_y_; y -= step) {
        if (aquifer.surface_density_->compute(FunctionContext{x, y, z}) > kSurfaceThreshold) {
            surface = y;
            break;
        }
    }
    surfaces_.emplace(key, surface);
    return surface;
}

FluidStatus AquiferSampler::status_of_cell(i32 gx, i32 gy, i32 gz) {
    const i64 key = pack(gx, gy, gz);
    if (const auto found = statuses_.find(key); found != statuses_.end()) {
        return found->second;
    }
    const auto        at     = aquifer_->centre(gx, gy, gz);
    const FluidStatus status = compute_status(at[0], at[1], at[2]);
    statuses_.emplace(key, status);
    return status;
}

FluidStatus AquiferSampler::compute_status(i32 x, i32 y, i32 z) {
    const Aquifer&    aquifer = *aquifer_;
    const FluidStatus global  = aquifer.global_fluid(y);

    // A centre in the lava layer takes the global rule outright ("Disabled,
    // if lava is selected", in the explanation).
    if (global.fluid == AquiferFluid::Lava) {
        return global;
    }

    // i64 because a column with no surface at all answers the largest i32,
    // and raising that by eight would wrap.
    i64  lowest    = std::numeric_limits<i64>::max();
    bool own_under = false;
    const i64 top    = static_cast<i64>(y) + kCellReach;
    const i64 bottom = static_cast<i64>(y) - kCellReach;

    for (const auto& offset : kSurfaceOffsets) {
        i32 sx = 0;
        i32 sz = 0;
        if (aquifer.tuning_.surface_from_centre) {
            sx = x + offset[0] * 16;
            sz = z + offset[1] * 16;
        } else {
            sx = (floor_div(x, 16) + offset[0]) * 16;
            sz = (floor_div(z, 16) + offset[1]) * 16;
        }
        const i32  surface = preliminary_surface(sx, sz);
        const i64  raised  = static_cast<i64>(surface) + kSurfaceRaise;
        const bool own     = offset[0] == 0 && offset[1] == 0;

        // 1. The bottom of the cell is above its own chunk's surface: the
        //    cell is in the open and follows the global rule.
        if (own && bottom > raised) {
            return global;
        }

        // 2. The top of the cell pokes above a surface that is under the sea:
        //    the cell is part of the sea.
        const bool top_above = top > raised;
        if ((top_above || own) && surface != kNoSurface) {
            const auto        raised_y = static_cast<i32>(raised);
            const FluidStatus there    = aquifer.global_fluid(raised_y);
            if (there.at(raised_y) != AquiferFluid::None) {
                if (own) {
                    own_under = true;
                }
                if (top_above) {
                    return there;
                }
            }
        }
        lowest = std::min(lowest, static_cast<i64>(surface));
    }

    // 3. Underground: the noise decides.
    const auto lowest_y = static_cast<i32>(std::min<i64>(lowest, kNoSurface - kSurfaceRaise));
    const i32  level    = surface_level(x, y, z, global, lowest_y, own_under);
    return {level, fluid_type(x, y, z, global, level)};
}

i32 AquiferSampler::surface_level(i32 x, i32 y, i32 z, FluidStatus global, i32 lowest_surface,
                                  bool own_under_sea) {
    const Aquifer&        aquifer = *aquifer_;
    const FunctionContext at{x, y, z};

    // The exclusion region is dry whatever the floodedness says.
    if (aquifer.erosion_->compute(at) < kExclusionErosion &&
        aquifer.depth_->compute(at) > kExclusionDepth) {
        return aquifer.dry_level();
    }

    // The thresholds slide from the sea-floor pair to the inland pair over the
    // 64 blocks under the lowest raised surface — but only where the cell's
    // own surface is under the sea; on land the depth does not matter.
    const i32 depth_below = lowest_surface + kSurfaceRaise - y;
    const f64 closeness =
        own_under_sea ? 1.0 - std::clamp(static_cast<f64>(depth_below) / 64.0, 0.0, 1.0) : 0.0;
    const f64 flooded   = lerp(closeness, 0.8, -0.3);
    const f64 partially = lerp(closeness, 0.4, -0.8);

    const f64 floodedness = std::clamp(aquifer.floodedness_->compute(at), -1.0, 1.0);
    if (floodedness > flooded) {
        return global.level;
    }
    if (floodedness > partially) {
        // Sampled at the index of a 16 x 40 x 16 grid, not at a block: the
        // three small integers are the coordinates the noise is asked at.
        const i32 gx     = floor_div(x, 16);
        const i32 gy     = floor_div(y, 40);
        const i32 gz     = floor_div(z, 16);
        const i32 middle = gy * 40 + 20;
        const f64 spread = aquifer.spread_->compute(FunctionContext{gx, gy, gz}) * 10.0;
        // "Rounded down to the next multiple of 3."
        const auto step = static_cast<i32>(std::floor(spread / 3.0)) * 3;
        const i32  cap  = aquifer.tuning_.cap_raised ? lowest_surface + kSurfaceRaise
                                                     : lowest_surface;
        return std::min(cap, middle + step);
    }
    return aquifer.dry_level();
}

AquiferFluid AquiferSampler::fluid_type(i32 x, i32 y, i32 z, FluidStatus global, i32 level) {
    const Aquifer& aquifer = *aquifer_;
    if (level <= -10 && level != aquifer.dry_level()) {
        // One sample per 64 x 40 x 64 region, at its index.
        const FunctionContext at{floor_div(x, 64), floor_div(y, 40), floor_div(z, 64)};
        if (std::abs(aquifer.lava_->compute(at)) > 0.3) {
            return AquiferFluid::Lava;
        }
    }
    return global.fluid;
}

f64 AquiferSampler::pressure(i32 x, i32 y, i32 z, FluidStatus first, FluidStatus second) {
    const AquiferTuning& tuning = aquifer_->tuning_;
    const AquiferFluid   a      = first.at(y);
    const AquiferFluid   b      = second.at(y);
    if ((a == AquiferFluid::Lava && b == AquiferFluid::Water) ||
        (a == AquiferFluid::Water && b == AquiferFluid::Lava)) {
        return tuning.water_lava;
    }
    const i32 gap = std::abs(first.level - second.level);
    if (gap == 0) {
        return 0.0;
    }
    const f64 middle   = 0.5 * static_cast<f64>(first.level + second.level);
    const f64 above    = static_cast<f64>(y) + 0.5 - middle;
    const f64 half     = static_cast<f64>(gap) / 2.0;
    const f64 distance = half - std::abs(above);

    f64 raw = 0.0;
    if (above > 0.0) {
        raw = distance > 0.0 ? distance / tuning.above_in : distance / tuning.above_out;
    } else {
        const f64 shifted = tuning.below_bias + distance;
        raw = shifted > 0.0 ? shifted / tuning.below_in : shifted / tuning.below_out;
    }

    f64 noise = 0.0;
    if (tuning.use_barrier_noise && raw >= -tuning.band && raw <= tuning.band) {
        if (!barrier_known_) {
            barrier_value_ = aquifer_->barrier_->compute(FunctionContext{x, y, z});
            barrier_known_ = true;
        }
        noise = barrier_value_;
    }
    return tuning.gain * (noise + raw);
}

AquiferAnswer AquiferSampler::compute(i32 x, i32 y, i32 z, f64 density) {
    if (density > 0.0) {
        return {Substance::Solid, false};
    }
    const Aquifer&    aquifer = *aquifer_;
    const FluidStatus global  = aquifer.global_fluid(y);

    // The lava layer exists regardless of aquifers.
    if (global.at(y) == AquiferFluid::Lava) {
        return {Substance::Lava, false};
    }
    if (!aquifer.enabled()) {
        return {substance_of(global.at(y)), false};
    }

    barrier_known_ = false;

    std::array<i32, 4>                distance{};
    std::array<std::array<i32, 3>, 4> cell{};
    nearest(x, y, z, distance, cell);

    const FluidStatus first = status_of_cell(cell[0][0], cell[0][1], cell[0][2]);
    const AquiferFluid here = first.at(y);

    const AquiferTuning& tuning = aquifer.tuning_;

    // Woken once the chunk exists: a fluid whose second centre is nearly as
    // close as its first, whatever the two statuses (measured, see the
    // tuning), and water directly above the lava layer, always.
    const bool above_lava =
        here == AquiferFluid::Water && aquifer.global_fluid(y - 1).at(y - 1) == AquiferFluid::Lava;
    const bool near_boundary = distance[1] - distance[0] < tuning.schedule_gap;
    const bool schedule = here != AquiferFluid::None && (near_boundary || above_lava);

    const f64 near_12 = similarity(distance[0], distance[1]);
    if (near_12 <= 0.0) {
        return {substance_of(here), schedule};
    }

    const FluidStatus    second = status_of_cell(cell[1][0], cell[1][1], cell[1][2]);

    if (density + near_12 * pressure(x, y, z, first, second) > 0.0) {
        return {Substance::Solid, false};
    }

    const FluidStatus third   = status_of_cell(cell[2][0], cell[2][1], cell[2][2]);
    const f64         near_13 = similarity(distance[0], distance[2]);
    if (near_13 > 0.0) {
        const f64 weight = tuning.chain_similarity ? near_12 * near_13 : near_13;
        if (density + weight * pressure(x, y, z, first, third) > 0.0) {
            return {Substance::Solid, false};
        }
    }
    const f64 near_23 = similarity(distance[1], distance[2]);
    if (near_23 > 0.0) {
        const f64 weight = tuning.chain_similarity ? near_12 * near_23 : near_23;
        if (density + weight * pressure(x, y, z, second, third) > 0.0) {
            return {Substance::Solid, false};
        }
    }

    return {substance_of(here), schedule};
}

AquiferNeighbourhood AquiferSampler::neighbourhood(i32 x, i32 y, i32 z) {
    std::array<std::array<i32, 3>, 4> cell{};
    AquiferNeighbourhood              out;
    nearest(x, y, z, out.distance, cell);
    for (usize k = 0; k < 4; ++k) {
        out.status[k] = status_of_cell(cell[k][0], cell[k][1], cell[k][2]);
    }
    return out;
}

void AquiferSampler::nearest(i32 x, i32 y, i32 z, std::array<i32, 4>& distance,
                             std::array<std::array<i32, 3>, 4>& cell) {
    const Aquifer& aquifer = *aquifer_;

    // The four nearest centres among the twelve cells around the block: its
    // own cell and the next one on x and z, the rows above and below on y.
    // Strict comparisons, so on a tie the centre met first stays ahead.
    const i32 gx0 = aquifer.grid_xz(x);
    const i32 gy0 = aquifer.grid_y(y);
    const i32 gz0 = aquifer.grid_xz(z);

    distance.fill(std::numeric_limits<i32>::max());

    for (i32 ix = 0; ix <= 1; ++ix) {
        for (i32 iy = -1; iy <= 1; ++iy) {
            for (i32 iz = 0; iz <= 1; ++iz) {
                const i32 gx  = gx0 + ix;
                const i32 gy  = gy0 + iy;
                const i32 gz  = gz0 + iz;
                const i64 key = pack(gx, gy, gz);
                auto      it  = centres_.find(key);
                if (it == centres_.end()) {
                    it = centres_.emplace(key, aquifer.centre(gx, gy, gz)).first;
                }
                const i32 dx = it->second[0] - x;
                const i32 dy = it->second[1] - y;
                const i32 dz = it->second[2] - z;
                const i32 d  = dx * dx + dy * dy + dz * dz;

                usize slot = 4;
                for (usize k = 0; k < 4; ++k) {
                    if (d < distance[k]) {
                        slot = k;
                        break;
                    }
                }
                if (slot == 4) {
                    continue;
                }
                for (usize k = 3; k > slot; --k) {
                    distance[k] = distance[k - 1];
                    cell[k]     = cell[k - 1];
                }
                distance[slot] = d;
                cell[slot]     = {gx, gy, gz};
            }
        }
    }
}

}  // namespace ov::worldgen
