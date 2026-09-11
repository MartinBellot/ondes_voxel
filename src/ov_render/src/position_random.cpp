#include "ov/render/position_random.hpp"

#include <algorithm>

namespace ov::render {

namespace {

constexpr u64 kMultiplier = 0x5DEECE66DULL;
constexpr u64 kAddend     = 0xBULL;
constexpr u64 kMask       = (1ULL << 48) - 1;

}  // namespace

i64 position_seed(i32 x, i32 y, i32 z) noexcept {
    // Unsigned arithmetic for the wrapping the hash relies on: x times its
    // constant wraps in 32 bits, the rest in 64. Signed overflow would be
    // undefined in C++ where it is defined in the game's language.
    const auto x_term = static_cast<i64>(static_cast<i32>(static_cast<u32>(x) * 3129871U));
    const auto z_term = static_cast<i64>(static_cast<u64>(static_cast<i64>(z)) * 116129781ULL);
    const auto l      = static_cast<u64>(x_term ^ z_term ^ static_cast<i64>(y));
    const u64  mixed  = l * l * 42317861ULL + l * 11ULL;
    return static_cast<i64>(mixed) >> 16;
}

LegacyLcg::LegacyLcg(i64 seed) noexcept : state_((static_cast<u64>(seed) ^ kMultiplier) & kMask) {}

i32 LegacyLcg::next(u32 bits) noexcept {
    state_ = (state_ * kMultiplier + kAddend) & kMask;
    return static_cast<i32>(static_cast<u32>(state_ >> (48 - bits)));
}

i64 LegacyLcg::next_long() noexcept {
    const i64 high = next(32);
    const i64 low  = next(32);
    return static_cast<i64>(static_cast<u64>(high) << 32) + low;
}

Vec3f block_offset(i32 x, i32 z, OffsetType type, f32 max_horizontal, f32 max_vertical) noexcept {
    if (type == OffsetType::None) {
        return Vec3f{};
    }
    const auto seed = static_cast<u64>(position_seed(x, 0, z));
    // A nibble over fifteen in single precision, then the rest in double:
    // the mix of widths the printed values carry (0.083333343, not 0.0833333).
    const auto nibble = [seed](u32 shift) {
        return static_cast<f64>(static_cast<f32>((seed >> shift) & 15U) / 15.0F);
    };
    const f64 limit = static_cast<f64>(max_horizontal);
    const f64 dx    = std::clamp((nibble(0) - 0.5) * 0.5, -limit, limit);
    const f64 dz    = std::clamp((nibble(8) - 0.5) * 0.5, -limit, limit);
    const f64 dy =
        type == OffsetType::XYZ ? (nibble(4) - 1.0) * static_cast<f64>(max_vertical) : 0.0;
    return Vec3f{static_cast<f32>(dx), static_cast<f32>(dy), static_cast<f32>(dz)};
}

u32 pick_weighted(i64 seed, std::span<const i32> weights) noexcept {
    if (weights.empty()) {
        return 0;
    }
    i64 total = 0;
    for (const i32 w : weights) {
        total += w < 1 ? 1 : w;
    }
    LegacyLcg  random(seed);
    const auto low = static_cast<i32>(static_cast<u32>(static_cast<u64>(random.next_long())));
    // The absolute value of the most negative int is itself, and stays
    // negative: the modulo then lands below zero, and the walk below takes the
    // first alternative — which is what the game's own walk does with it.
    const i64 magnitude = low == INT32_MIN ? static_cast<i64>(low) : (low < 0 ? -low : low);
    i64       index     = magnitude % total;
    for (u32 i = 0; i < weights.size(); ++i) {
        index -= weights[i] < 1 ? 1 : weights[i];
        if (index < 0) {
            return i;
        }
    }
    return 0;
}

}  // namespace ov::render
