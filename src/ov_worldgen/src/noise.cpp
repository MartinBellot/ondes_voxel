#include "ov/worldgen/noise.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

namespace ov::worldgen {

namespace {

/// The scale every old terrain noise is measured in. Not derived from
/// anything — it is the constant the 1.17 generator used, and the terrain's
/// horizontal wavelength is set by it.
constexpr f64 kOldNoiseScale = 684.412;

/// Sixteen octaves in each limit stack, and the divisors that undo their
/// weights.
///
/// These three numbers were doubted together. The octaves *grow* — octave i is
/// divided by a falloff that halves, so it contributes up to 2 * 2^i and the
/// sum over sixteen is 131070, not the 4 a halving series would give — and
/// 512 * 128 is 65536, which is exactly what it takes to bring that sum back to
/// about two. Dropping two octaves, or making the second divisor 512, each
/// divides the result by four; a surface histogram in the overworld liked the
/// smaller field, and for a while that looked like evidence.
///
/// It was not. The Nether's noise_settings put this noise on its own — no
/// depth, no factor, no aquifers — with a threshold that slides one step per
/// block above y = 104, so the fraction of stone at each height there is this
/// noise's own survival function as the game writes it. Measured that way on
/// two seeds (`ov_parity --nether`), the game's distribution is as wide as the
/// one below and about four times wider than either quarter-size candidate.
/// The three constants stay.
constexpr usize kLimitOctaves  = 16;
constexpr f64   kFirstDivisor  = 512.0;
constexpr f64   kSecondDivisor = 128.0;

}  // namespace

BlendedNoise::LegacyStack BlendedNoise::LegacyStack::create(math::XoroshiroRandomSource& random,
                                                            i32 first_octave, usize count) {
    LegacyStack stack;
    stack.first_octave = first_octave;
    stack.octaves.resize(count);

    // Sequential, and in this order. The octave at index `-first_octave` — the
    // highest frequency — is created first, then the rest descend to index
    // zero. Creating them in array order instead would draw the same numbers
    // in a different arrangement and give a different world.
    const auto top = static_cast<usize>(-first_octave);
    if (top < count) {
        stack.octaves[top] = std::make_unique<ImprovedNoise>(random);
    }
    for (usize index = top; index-- > 0;) {
        stack.octaves[index] = std::make_unique<ImprovedNoise>(random);
    }
    return stack;
}

BlendedNoise BlendedNoise::create(math::XoroshiroRandomSource& random, f64 xz_scale, f64 y_scale,
                                  f64 xz_factor, f64 y_factor, f64 smear_scale_multiplier) {
    BlendedNoise noise;
    // All three from the same generator, in this order: the two limits at
    // sixteen octaves each, then the selector at eight.
    constexpr auto kLimitFirst = -static_cast<i32>(kLimitOctaves) + 1;
    noise.min_limit_ = LegacyStack::create(random, kLimitFirst, kLimitOctaves);
    noise.max_limit_ = LegacyStack::create(random, kLimitFirst, kLimitOctaves);
    noise.main_      = LegacyStack::create(random, -7, 8);

    noise.xz_multiplier_          = kOldNoiseScale * xz_scale;
    noise.y_multiplier_           = kOldNoiseScale * y_scale;
    noise.xz_factor_              = xz_factor;
    noise.y_factor_               = y_factor;
    noise.smear_scale_multiplier_ = smear_scale_multiplier;

    // The bound, and it is worth being careful because it is not a decoration:
    // the density graph uses min_value and max_value to decide whether a `min`
    // or a `max` may skip its second argument entirely, so a bound that is too
    // small makes those nodes return the wrong number rather than merely being
    // slow.
    //
    // The octaves *grow*. Each is divided by its falloff, which halves, so
    // octave i contributes up to 2 * 2^i — the sum over sixteen is 131070, not
    // the 4 a halving series would give. Divided by 512 and then by 128 that is
    // about two, which is the scale terrain density actually works at; the
    // first version of this line produced six hundredths of a thousandth.
    f64 bound = 0.0;
    f64 weight = 1.0;
    for (usize i = 0; i < kLimitOctaves; ++i) {
        bound += 2.0 * weight;
        weight *= 2.0;
    }
    noise.max_value_ = bound / kFirstDivisor / kSecondDivisor;

    // OV_SELECTOR_DIV divides the selector, and nothing else, before the blend.
    //
    // It exists to settle one suspicion by measurement rather than by argument.
    // The blend is `(selector / 10 + 1) / 2` while the selector is eight
    // octaves whose weights double, so it reaches far past ten and the blend
    // clamps: the crossfade between the two limit stacks becomes a switch. That
    // was written down as a possible bug and never tested. Dividing the
    // selector is exactly the "the game normalises this stack" hypothesis, and
    // the Nether — where this noise is the whole of the density — reads the
    // resulting distribution against the game's own blocks. Default 1.0, which
    // is the field as it stands.
    if (const char* text = std::getenv("OV_SELECTOR_DIV"); text != nullptr) {
        const f64 divisor = std::strtod(text, nullptr);
        if (divisor > 0.0) {
            noise.selector_divisor_ = divisor;
        }
    }
    return noise;
}

f64 BlendedNoise::selector(i32 x, i32 y, i32 z) const noexcept {
    const f64 sx = static_cast<f64>(x) * xz_multiplier_;
    const f64 sy = static_cast<f64>(y) * y_multiplier_;
    const f64 sz = static_cast<f64>(z) * xz_multiplier_;

    // The selector is sampled at a coarser scale than the limits, which is why
    // it varies slowly enough to choose between them over whole hillsides
    // rather than block by block.
    const f64 mx = sx / xz_factor_;
    const f64 my = sy / y_factor_;
    const f64 mz = sz / xz_factor_;

    const f64 main_smear = y_multiplier_ * smear_scale_multiplier_ / y_factor_;

    f64 total   = 0.0;
    f64 falloff = 1.0;
    for (const auto& octave : main_.octaves) {
        if (octave != nullptr) {
            total += octave->noise(PerlinNoise::wrap(mx * falloff),
                                   PerlinNoise::wrap(my * falloff),
                                   PerlinNoise::wrap(mz * falloff), main_smear * falloff,
                                   my * falloff) /
                     falloff;
        }
        falloff /= 2.0;
    }
    return total;
}

f64 BlendedNoise::value(i32 x, i32 y, i32 z) const noexcept {
    const f64 sx = static_cast<f64>(x) * xz_multiplier_;
    const f64 sy = static_cast<f64>(y) * y_multiplier_;
    const f64 sz = static_cast<f64>(z) * xz_multiplier_;

    const f64 smear = y_multiplier_ * smear_scale_multiplier_;

    const f64 blend = (selector(x, y, z) / selector_divisor_ / 10.0 + 1.0) / 2.0;
    // Saturated on either side the other stack is never touched. Not only an
    // optimisation: it is sixteen octaves of Perlin skipped for most of the
    // world.
    const bool only_max = blend >= 1.0;
    const bool only_min = blend <= 0.0;

    f64 low     = 0.0;
    f64 high    = 0.0;
    f64 falloff = 1.0;
    for (usize index = 0; index < min_limit_.octaves.size(); ++index) {
        const f64 wx = PerlinNoise::wrap(sx * falloff);
        const f64 wy = PerlinNoise::wrap(sy * falloff);
        const f64 wz = PerlinNoise::wrap(sz * falloff);
        const f64 sm = smear * falloff;
        if (!only_max && min_limit_.octaves[index] != nullptr) {
            low += min_limit_.octaves[index]->noise(wx, wy, wz, sm, sy * falloff) / falloff;
        }
        if (!only_min && max_limit_.octaves[index] != nullptr) {
            high += max_limit_.octaves[index]->noise(wx, wy, wz, sm, sy * falloff) / falloff;
        }
        falloff /= 2.0;
    }

    const f64 t = std::clamp(blend, 0.0, 1.0);
    const f64 lower = low / kFirstDivisor;
    const f64 upper = high / kFirstDivisor;
    return (lower + (upper - lower) * t) / kSecondDivisor;
}

}  // namespace ov::worldgen
