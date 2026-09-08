#include "ov/worldgen/noise.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace ov::worldgen {

namespace {

/// The sixteen gradient directions. Twelve distinct ones over the edges of a
/// cube, and four repeats so that the index can be masked with 15 instead of
/// taken modulo 12 — which is the trick that makes the lookup a mask.
constexpr std::array<std::array<i8, 3>, 16> kGradients{{
    {1, 1, 0},  {-1, 1, 0},  {1, -1, 0},  {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},   {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
    {1, 1, 0},  {0, -1, 1},  {-1, 1, 0},  {0, -1, -1},
}};

[[nodiscard]] constexpr i32 floor_i32(f64 value) noexcept {
    const auto truncated = static_cast<i32>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

[[nodiscard]] constexpr i64 floor_i64(f64 value) noexcept {
    const auto truncated = static_cast<i64>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

/// Perlin's improved fade curve. Not smoothstep: this is the fifth-order one
/// with a zero second derivative at both ends, which is what keeps the grid
/// from showing.
[[nodiscard]] constexpr f64 fade(f64 t) noexcept {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] constexpr f64 lerp(f64 t, f64 a, f64 b) noexcept {
    return a + t * (b - a);
}

[[nodiscard]] constexpr f64 gradient_dot(u8 hash, f64 x, f64 y, f64 z) noexcept {
    const auto& g = kGradients[hash & 15U];
    return static_cast<f64>(g[0]) * x + static_cast<f64>(g[1]) * y + static_cast<f64>(g[2]) * z;
}

}  // namespace

template<typename Random>
void ImprovedNoise::seed_from(Random& random) {
    // Three offsets first, then the shuffle. The order is state, so drawing
    // them in any other one gives a different world.
    xo_ = random.next_double() * 256.0;
    yo_ = random.next_double() * 256.0;
    zo_ = random.next_double() * 256.0;

    for (usize i = 0; i < 256; ++i) {
        permutation_[i] = static_cast<u8>(i);
    }
    // A partial Fisher-Yates: the swap partner is drawn from what is *left*,
    // and the index is relative to i rather than absolute. Drawing from the
    // whole array instead is a different shuffle and a different world.
    for (usize i = 0; i < 256; ++i) {
        const auto j = static_cast<usize>(random.next_int(static_cast<i32>(256 - i)));
        std::swap(permutation_[i], permutation_[i + j]);
    }
}

ImprovedNoise::ImprovedNoise(math::XoroshiroRandomSource& random) {
    seed_from(random);
}

ImprovedNoise::ImprovedNoise(math::LegacyRandomSource& random) {
    seed_from(random);
}

f64 ImprovedNoise::noise(f64 x, f64 y, f64 z, f64 y_scale, f64 y_max) const noexcept {
    const f64 sx = x + xo_;
    const f64 sy = y + yo_;
    const f64 sz = z + zo_;

    const i32 ix = floor_i32(sx);
    const i32 iy = floor_i32(sy);
    const i32 iz = floor_i32(sz);

    const f64 dx = sx - static_cast<f64>(ix);
    const f64 dy = sy - static_cast<f64>(iy);
    const f64 dz = sz - static_cast<f64>(iz);

    // The y flattening: the sample's y offset is snapped to a multiple of
    // y_scale, which turns a smooth slope into terraces. It is what makes the
    // terrain noise produce cliffs rather than hills.
    f64 flattened = 0.0;
    if (y_scale != 0.0) {
        const f64 limit = (y_max >= 0.0 && y_max < dy) ? y_max : dy;
        flattened       = std::floor(limit / y_scale + 1.0E-7) * y_scale;
    }

    const f64 gy = dy - flattened;

    const u8 a  = permute(ix);
    const u8 b  = permute(ix + 1);
    const u8 aa = permute(static_cast<i32>(a) + iy);
    const u8 ab = permute(static_cast<i32>(a) + iy + 1);
    const u8 ba = permute(static_cast<i32>(b) + iy);
    const u8 bb = permute(static_cast<i32>(b) + iy + 1);

    const f64 c000 = gradient_dot(permute(static_cast<i32>(aa) + iz), dx, gy, dz);
    const f64 c100 = gradient_dot(permute(static_cast<i32>(ba) + iz), dx - 1.0, gy, dz);
    const f64 c010 = gradient_dot(permute(static_cast<i32>(ab) + iz), dx, gy - 1.0, dz);
    const f64 c110 = gradient_dot(permute(static_cast<i32>(bb) + iz), dx - 1.0, gy - 1.0, dz);
    const f64 c001 = gradient_dot(permute(static_cast<i32>(aa) + iz + 1), dx, gy, dz - 1.0);
    const f64 c101 = gradient_dot(permute(static_cast<i32>(ba) + iz + 1), dx - 1.0, gy, dz - 1.0);
    const f64 c011 = gradient_dot(permute(static_cast<i32>(ab) + iz + 1), dx, gy - 1.0, dz - 1.0);
    const f64 c111 =
        gradient_dot(permute(static_cast<i32>(bb) + iz + 1), dx - 1.0, gy - 1.0, dz - 1.0);

    // Note the fade is taken of `dy`, not of the flattened `gy`. The
    // interpolation weight follows the real position; only the gradient
    // sample is snapped.
    const f64 u = fade(dx);
    const f64 v = fade(dy);
    const f64 w = fade(dz);

    return lerp(w, lerp(v, lerp(u, c000, c100), lerp(u, c010, c110)),
                lerp(v, lerp(u, c001, c101), lerp(u, c011, c111)));
}

f64 PerlinNoise::wrap(f64 value) noexcept {
    return value - static_cast<f64>(floor_i64(value / 3.3554432E7 + 0.5)) * 3.3554432E7;
}

PerlinNoise PerlinNoise::create(math::XoroshiroRandomSource& random, i32 first_octave,
                                std::span<const f64> amplitudes) {
    PerlinNoise noise;
    noise.first_octave_ = first_octave;
    noise.amplitudes_.assign(amplitudes.begin(), amplitudes.end());
    noise.octaves_.resize(amplitudes.size());

    // Each octave is seeded by *name*, not in sequence. That is why a zero
    // amplitude can skip an octave without shifting the others: the name
    // carries the octave number.
    const math::XoroshiroPositionalFactory factory{static_cast<u64>(random.next_long()),
                                                   static_cast<u64>(random.next_long())};
    for (usize i = 0; i < amplitudes.size(); ++i) {
        if (amplitudes[i] == 0.0) {
            continue;
        }
        const std::string name = "octave_" + std::to_string(first_octave + static_cast<i32>(i));
        auto               source = factory.from_hash_of(name);
        noise.octaves_[i]         = std::make_unique<ImprovedNoise>(source);
    }

    const auto count           = static_cast<i32>(amplitudes.size());
    noise.lowest_input_factor_ = std::pow(2.0, static_cast<f64>(first_octave));
    // Normalises the stack so that the sum of a full set of octaves stays in
    // roughly the same range whatever the count.
    noise.lowest_value_factor_ =
        std::pow(2.0, static_cast<f64>(count - 1)) / (std::pow(2.0, static_cast<f64>(count)) - 1.0);

    f64 maximum = 0.0;
    f64 factor  = noise.lowest_value_factor_;
    for (usize i = 0; i < amplitudes.size(); ++i) {
        if (noise.octaves_[i] != nullptr) {
            // One octave of Perlin is bounded by 2 in the improved formulation.
            maximum += amplitudes[i] * 2.0 * factor;
        }
        factor /= 2.0;
    }
    noise.max_value_ = maximum;
    return noise;
}

f64 PerlinNoise::value(f64 x, f64 y, f64 z, f64 y_scale, f64 y_max, bool use_origin) const noexcept {
    f64 total  = 0.0;
    f64 input  = lowest_input_factor_;
    f64 output = lowest_value_factor_;

    for (usize i = 0; i < octaves_.size(); ++i) {
        const ImprovedNoise* octave = octaves_[i].get();
        if (octave != nullptr) {
            // `use_origin` samples at the octave's own y offset rather than at
            // the requested y — that is how a two-dimensional noise is taken
            // from a three-dimensional one without every octave agreeing.
            const f64 sampled =
                octave->noise(wrap(x * input), use_origin ? -octave->origin_y() : wrap(y * input),
                              wrap(z * input), y_scale * input, y_max * input);
            total += amplitudes_[i] * sampled * output;
        }
        input *= 2.0;
        output /= 2.0;
    }
    return total;
}

f64 PerlinNoise::max_value() const noexcept {
    return max_value_;
}

NormalNoise NormalNoise::create(math::XoroshiroRandomSource& random, i32 first_octave,
                                std::span<const f64> amplitudes) {
    // Both stacks are drawn from the same generator, one after the other, so
    // the order is state again.
    PerlinNoise first  = PerlinNoise::create(random, first_octave, amplitudes);
    PerlinNoise second = PerlinNoise::create(random, first_octave, amplitudes);

    // The scaling depends only on the *span* of non-zero octaves, not on how
    // many there are inside it.
    i32 lowest  = std::numeric_limits<i32>::max();
    i32 highest = std::numeric_limits<i32>::min();
    for (usize i = 0; i < amplitudes.size(); ++i) {
        if (amplitudes[i] != 0.0) {
            lowest  = std::min(lowest, static_cast<i32>(i));
            highest = std::max(highest, static_cast<i32>(i));
        }
    }
    const f64 span      = static_cast<f64>(highest - lowest);
    const f64 deviation = 0.1 * (1.0 + 1.0 / (span + 1.0));
    const f64 factor    = (1.0 / 6.0) / deviation;

    const f64 maximum = (first.max_value() + second.max_value()) * factor;
    return NormalNoise{std::move(first), std::move(second), factor, maximum};
}

f64 NormalNoise::value(f64 x, f64 y, f64 z) const noexcept {
    // The second stack is sampled at a scale that is deliberately not a round
    // multiple of the first: at the same scale the two would correlate and
    // their sum would show the grid.
    return (first_.value(x, y, z) +
            second_.value(x * kInputFactor, y * kInputFactor, z * kInputFactor)) *
           value_factor_;
}

}  // namespace ov::worldgen
