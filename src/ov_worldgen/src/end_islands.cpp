// The End's island noise, its biome rule and its spikes. See end.hpp.
#include "ov/worldgen/end.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace ov::worldgen {

namespace {

/// The skew and unskew factors of two-dimensional simplex noise, in doubles,
/// computed the way the specification writes them.
const f64 kF2 = 0.5 * (std::sqrt(3.0) - 1.0);
const f64 kG2 = (3.0 - std::sqrt(3.0)) / 6.0;

/// The gradients. Only the first twelve are reached (`% 12`).
constexpr std::array<std::array<i32, 3>, 16> kGradient{{{1, 1, 0},
                                                        {-1, 1, 0},
                                                        {1, -1, 0},
                                                        {-1, -1, 0},
                                                        {1, 0, 1},
                                                        {-1, 0, 1},
                                                        {1, 0, -1},
                                                        {-1, 0, -1},
                                                        {0, 1, 1},
                                                        {0, -1, 1},
                                                        {0, 1, -1},
                                                        {0, -1, -1},
                                                        {1, 1, 0},
                                                        {0, -1, 1},
                                                        {-1, 1, 0},
                                                        {0, -1, -1}}};

/// `Mth.floor` for a double.
[[nodiscard]] i32 floor_to_int(f64 value) noexcept {
    const auto truncated = static_cast<i32>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

/// One corner's contribution. The subtractions are left to right, as written.
[[nodiscard]] f64 corner(i32 index, f64 x, f64 y, f64 z, f64 base) noexcept {
    f64 falloff = base - x * x;
    falloff     = falloff - y * y;
    falloff     = falloff - z * z;
    if (falloff < 0.0) {
        return 0.0;
    }
    falloff *= falloff;
    const auto& g   = kGradient[static_cast<usize>(index)];
    f64         dot = static_cast<f64>(g[0]) * x;
    dot             = dot + static_cast<f64>(g[1]) * y;
    dot             = dot + static_cast<f64>(g[2]) * z;
    return falloff * falloff * dot;
}

[[nodiscard]] f32 clamp_f(f32 value, f32 low, f32 high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

/// The router's node: `EndIslands`, with a memo per eighth-scale column.
class EndIslandsNode final : public DensityFunction {
public:
    explicit EndIslandsNode(i64 seed) : islands_(seed) {}

    [[nodiscard]] f64 compute(const FunctionContext& at) const override {
        // The height is a function of (x / 8, z / 8) alone, and the router asks
        // a column many times over — every y of every cell corner. A small
        // direct-mapped memo, `mutable` like every cache in a router (one stack
        // per thread; see generated_world.cpp).
        const i32   sx   = at.x / 8;
        const i32   sz   = at.z / 8;
        const usize slot = (static_cast<usize>(static_cast<u32>(sx) * 31U) ^
                            static_cast<usize>(static_cast<u32>(sz) * 17U)) &
                           (kMemo - 1);
        Memo& memo = memo_[slot];
        if (!memo.valid || memo.x != sx || memo.z != sz) {
            memo.value = (static_cast<f64>(islands_.height_value(sx, sz)) - 8.0) / 128.0;
            memo.x     = sx;
            memo.z     = sz;
            memo.valid = true;
        }
        return memo.value;
    }

    [[nodiscard]] f64 min_value() const override { return EndIslands::kMinValue; }
    [[nodiscard]] f64 max_value() const override { return EndIslands::kMaxValue; }

private:
    static constexpr usize kMemo = 256;
    struct Memo {
        i32  x{0};
        i32  z{0};
        f64  value{0.0};
        bool valid{false};
    };

    EndIslands                        islands_;
    mutable std::array<Memo, kMemo>   memo_{};
};

}  // namespace

// ── SimplexNoise ────────────────────────────────────────────────────────────

SimplexNoise::SimplexNoise(math::LegacyRandomSource& random) {
    xo_ = random.next_double() * 256.0;
    yo_ = random.next_double() * 256.0;
    zo_ = random.next_double() * 256.0;
    for (usize i = 0; i < 256; ++i) {
        permutation_[i] = static_cast<i32>(i);
    }
    for (usize i = 0; i < 256; ++i) {
        const auto j = static_cast<usize>(random.next_int(static_cast<i32>(256 - i)));
        std::swap(permutation_[i], permutation_[i + j]);
    }
}

f64 SimplexNoise::value(f64 x, f64 y) const noexcept {
    const f64 skew = (x + y) * kF2;
    const i32 i    = floor_to_int(x + skew);
    const i32 j    = floor_to_int(y + skew);
    const f64 un   = static_cast<f64>(i + j) * kG2;
    const f64 x0o  = static_cast<f64>(i) - un;
    const f64 y0o  = static_cast<f64>(j) - un;
    const f64 x0   = x - x0o;
    const f64 y0   = y - y0o;

    i32 step_x = 0;
    i32 step_y = 0;
    if (x0 > y0) {
        step_x = 1;
    } else {
        step_y = 1;
    }

    const f64 x1 = x0 - static_cast<f64>(step_x) + kG2;
    const f64 y1 = y0 - static_cast<f64>(step_y) + kG2;
    const f64 x2 = x0 - 1.0 + 2.0 * kG2;
    const f64 y2 = y0 - 1.0 + 2.0 * kG2;

    const i32 ii = i & 255;
    const i32 jj = j & 255;
    const i32 g0 = permute(ii + permute(jj)) % 12;
    const i32 g1 = permute(ii + step_x + permute(jj + step_y)) % 12;
    const i32 g2 = permute(ii + 1 + permute(jj + 1)) % 12;

    const f64 n0 = corner(g0, x0, y0, 0.0, 0.5);
    const f64 n1 = corner(g1, x1, y1, 0.0, 0.5);
    const f64 n2 = corner(g2, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);
}

// ── EndIslands ──────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] math::LegacyRandomSource island_random(i64 seed) {
    math::LegacyRandomSource random{seed};
    for (i32 i = 0; i < EndIslands::kDiscardedDraws; ++i) {
        (void)random.next_int();
    }
    return random;
}

}  // namespace

EndIslands::EndIslands(i64 seed)
    : noise_([&] {
          auto random = island_random(seed);
          return SimplexNoise{random};
      }()) {}

f32 EndIslands::height_value(i32 x, i32 z) const noexcept {
    // Truncating division and remainder, as Java's: -3 / 2 is -1 and -3 % 2 is -1.
    const i32 half_x = x / 2;
    const i32 half_z = z / 2;
    const i32 odd_x  = x % 2;
    const i32 odd_z  = z % 2;

    // The main island: a cone round the origin. The square is taken in ints,
    // as written; it would overflow only past 370 000 blocks.
    const f32 centre = static_cast<f32>(std::sqrt(static_cast<f32>(x * x + z * z)));
    f32       height = 100.0F - centre * 8.0F;
    height           = clamp_f(height, -100.0F, 80.0F);

    for (i32 dx = -12; dx <= 12; ++dx) {
        for (i32 dz = -12; dz <= 12; ++dz) {
            const i64 cx = static_cast<i64>(half_x) + dx;
            const i64 cz = static_cast<i64>(half_z) + dz;
            if (cx * cx + cz * cz <= 4096 ||
                noise_.value(static_cast<f64>(cx), static_cast<f64>(cz)) >=
                    static_cast<f64>(-0.9F)) {
                continue;
            }
            // How steep this island is: a hash of its own coordinates, in floats.
            const f32 ax        = std::abs(static_cast<f32>(cx)) * 3439.0F;
            const f32 az        = std::abs(static_cast<f32>(cz)) * 147.0F;
            const f32 steepness = std::fmod(ax + az, 13.0F) + 9.0F;
            const auto hx       = static_cast<f32>(odd_x - dx * 2);
            const auto hz       = static_cast<f32>(odd_z - dz * 2);
            const f32 distance  = static_cast<f32>(std::sqrt(hx * hx + hz * hz));
            f32       island    = 100.0F - distance * steepness;
            island              = clamp_f(island, -100.0F, 80.0F);
            height              = std::max(height, island);
        }
    }
    return height;
}

f64 EndIslands::density(i32 block_x, i32 block_z) const noexcept {
    return (static_cast<f64>(height_value(block_x / 8, block_z / 8)) - 8.0) / 128.0;
}

DensityRef make_end_islands_density(i64 seed) {
    return std::make_shared<const EndIslandsNode>(seed);
}

// ── The biome rule ──────────────────────────────────────────────────────────

usize end_biome_for_erosion(f64 erosion) noexcept {
    if (erosion > 0.25) {
        return 1;  // end_highlands
    }
    if (erosion >= -0.0625) {
        return 2;  // end_midlands
    }
    if (erosion < -0.21875) {
        return 3;  // small_end_islands
    }
    return 4;  // end_barrens
}

usize end_biome_at(const NoiseRouter& router, i32 quart_x, i32 quart_z) {
    const i32 block_x = quart_x * 4;
    const i32 block_z = quart_z * 4;
    const i64 chunk_x = block_x >> 4;
    const i64 chunk_z = block_z >> 4;
    if (chunk_x * chunk_x + chunk_z * chunk_z <= kEndCentreChunksSquared) {
        return 0;  // the_end
    }
    // The centre of the chunk: (section * 2 + 1) * 8.
    const i32 centre_x = (static_cast<i32>(chunk_x) * 2 + 1) * 8;
    const i32 centre_z = (static_cast<i32>(chunk_z) * 2 + 1) * 8;
    const DensityFunction* erosion = router.entry("erosion");
    if (erosion == nullptr) {
        return 0;
    }
    return end_biome_for_erosion(erosion->compute(FunctionContext{centre_x, 0, centre_z}));
}

// ── The spikes ──────────────────────────────────────────────────────────────

std::array<EndSpike, 10> end_spikes(i64 level_seed) {
    math::LegacyRandomSource seeder{level_seed};
    const i64                key = seeder.next_long() & 65535;

    // `Collections.shuffle(list, new Random(key))`: from the back, each element
    // swapped with one drawn from those not yet fixed.
    std::array<i32, 10> order{};
    for (i32 i = 0; i < 10; ++i) {
        order[static_cast<usize>(i)] = i;
    }
    math::LegacyRandomSource shuffler{key};
    for (i32 i = 10; i > 1; --i) {
        const i32 j = shuffler.next_int(i);
        std::swap(order[static_cast<usize>(i - 1)], order[static_cast<usize>(j)]);
    }

    std::array<EndSpike, 10> spikes{};
    for (i32 i = 0; i < 10; ++i) {
        const f64 angle = 2.0 * (-std::numbers::pi + (std::numbers::pi / 10.0) * static_cast<f64>(i));
        EndSpike& spike = spikes[static_cast<usize>(i)];
        spike.centre_x  = floor_to_int(42.0 * std::cos(angle));
        spike.centre_z  = floor_to_int(42.0 * std::sin(angle));
        const i32 index = order[static_cast<usize>(i)];
        spike.radius    = 2 + index / 3;
        spike.height    = 76 + index * 3;
        spike.guarded   = index == 1 || index == 2;
    }
    return spikes;
}

}  // namespace ov::worldgen
