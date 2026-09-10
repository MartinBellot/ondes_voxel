// The noise the terrain is made of.
//
// Four layers, each built on the one below:
//
//   ImprovedNoise   one octave of Perlin, over a permutation the RNG shuffles
//   PerlinNoise     a stack of octaves at doubling frequencies
//   NormalNoise     two Perlin stacks, offset and scaled to a known deviation
//   BlendedNoise    the old 1.17 terrain noise, still used by the router
//
// Everything here is bit-sensitive. The whole point of seed parity is that the
// same seed produces the same world, and a rounding difference in any of these
// moves every hill. So: doubles throughout, no reassociation, and the build
// already forbids -ffast-math and sets -ffp-contract=off.
//
// Provenance: these algorithms are documented in permissively licensed
// reimplementations and in the technical community's write-ups, never read from
// Mojang's code. They are verified the way everything else in this project is —
// against what the real game writes to disk for the same seed. See
// docs/PROVENANCE.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace ov::worldgen {

/// One octave of Perlin noise.
class ImprovedNoise {
public:
    explicit ImprovedNoise(math::XoroshiroRandomSource& random);
    explicit ImprovedNoise(math::LegacyRandomSource& random);

    [[nodiscard]] f64 noise(f64 x, f64 y, f64 z) const noexcept {
        return noise(x, y, z, 0.0, 0.0);
    }

    /// The two extra arguments flatten the sample in y, which the terrain
    /// noise uses to make cliffs. Zero means no flattening.
    [[nodiscard]] f64 noise(f64 x, f64 y, f64 z, f64 y_scale, f64 y_max) const noexcept;

    [[nodiscard]] f64 origin_y() const noexcept { return yo_; }

private:
    template<typename Random>
    void seed_from(Random& random);

    [[nodiscard]] u8 permute(i32 value) const noexcept {
        return permutation_[static_cast<usize>(value & 255)];
    }

    f64                  xo_{0.0};
    f64                  yo_{0.0};
    f64                  zo_{0.0};
    std::array<u8, 256>  permutation_{};
};

/// A stack of octaves at doubling frequencies, with per-octave amplitudes.
///
/// The amplitudes come straight from the datapack: `firstOctave` says which
/// frequency the list starts at, and a zero amplitude means that octave is not
/// even created — which matters, because creating it would consume a name from
/// the factory and shift every octave after it.
class PerlinNoise {
public:
    [[nodiscard]] static PerlinNoise create(math::XoroshiroRandomSource& random, i32 first_octave,
                                            std::span<const f64> amplitudes);

    /// The same "new" initialisation — every octave seeded by name from a
    /// positional factory forked off `random` — for a world whose settings say
    /// `legacy_random_source: true`. The Nether's are. The fork takes one long
    /// rather than two and the names hash with Java's string hash rather than
    /// MD5, which is all that differs; the octave layout is the same.
    [[nodiscard]] static PerlinNoise create(math::LegacyRandomSource& random, i32 first_octave,
                                            std::span<const f64> amplitudes);

    /// The *old* initialisation: octaves drawn in sequence from `random` itself,
    /// the one at octave zero first and then downwards, with 262 draws skipped
    /// for every octave that is out of range or has a zero amplitude.
    ///
    /// Only one thing in 1.20.1 still asks for it through a named noise: the
    /// Nether's temperature and vegetation, which the game builds from
    /// `new LegacyRandomSource(seed)` and `seed + 1` rather than from their JSON
    /// files. The skip is not an optimisation: the first `ImprovedNoise` is
    /// constructed — and consumes its draws — even when its octave is not kept.
    [[nodiscard]] static PerlinNoise create_legacy(math::LegacyRandomSource& random,
                                                   i32 first_octave,
                                                   std::span<const f64> amplitudes);

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z) const noexcept {
        return value(x, y, z, 0.0, 0.0, false);
    }

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z, f64 y_scale, f64 y_max,
                            bool use_origin) const noexcept;

    /// The largest absolute value this stack can produce. The density function
    /// graph uses it to bound whole subtrees without evaluating them.
    [[nodiscard]] f64 max_value() const noexcept;

    /// Fold a coordinate back near the origin.
    ///
    /// Perlin's permutation repeats every 256 cells, and a double loses its
    /// fractional precision long before the coordinate gets large. Vanilla
    /// wraps at 2^25 * 4 so that a world border's worth of terrain still has
    /// the same shape it would near the origin.
    [[nodiscard]] static f64 wrap(f64 value) noexcept;

private:
    /// Everything but the octaves: the input and value factors and the bound.
    /// Shared by the three constructors, which differ only in how the octaves
    /// were seeded.
    [[nodiscard]] static PerlinNoise from_octaves(
        std::vector<std::unique_ptr<ImprovedNoise>> octaves, i32 first_octave,
        std::span<const f64> amplitudes);

    std::vector<std::unique_ptr<ImprovedNoise>> octaves_;
    std::vector<f64>                            amplitudes_;
    i32                                         first_octave_{0};
    f64                                         lowest_input_factor_{0.0};
    f64                                         lowest_value_factor_{0.0};
    f64                                         max_value_{0.0};
};

/// Two Perlin stacks, the second sampled at a slightly different scale, scaled
/// so the result has a predictable deviation.
///
/// The second stack's input factor is not a round number and not arbitrary:
/// sampling both at the same scale would make them correlate and the sum would
/// have visible structure.
class NormalNoise {
public:
    /// Vanilla's second-sample input factor.
    static constexpr f64 kInputFactor = 1.0181268882175227;

    [[nodiscard]] static NormalNoise create(math::XoroshiroRandomSource& random, i32 first_octave,
                                            std::span<const f64> amplitudes);

    /// The same for a legacy-seeded world: both stacks by the new
    /// initialisation, drawn from a legacy generator. See PerlinNoise.
    [[nodiscard]] static NormalNoise create(math::LegacyRandomSource& random, i32 first_octave,
                                            std::span<const f64> amplitudes);

    /// `createLegacyNetherBiome`: both stacks by the *old* initialisation.
    /// What the Nether's temperature and vegetation are, whatever their JSON
    /// files say.
    [[nodiscard]] static NormalNoise create_legacy_nether_biome(
        math::LegacyRandomSource& random, i32 first_octave, std::span<const f64> amplitudes);

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z) const noexcept;

    [[nodiscard]] f64 max_value() const noexcept { return max_value_; }

private:
    [[nodiscard]] static NormalNoise from_stacks(PerlinNoise first, PerlinNoise second,
                                                 std::span<const f64> amplitudes);

    PerlinNoise first_;
    PerlinNoise second_;
    f64         value_factor_{0.0};
    f64         max_value_{0.0};

    NormalNoise(PerlinNoise first, PerlinNoise second, f64 value_factor, f64 max_value)
        : first_(std::move(first)),
          second_(std::move(second)),
          value_factor_(value_factor),
          max_value_(max_value) {}
};

/// The terrain noise from before 1.18, still the backbone of the shape.
///
/// The density function graph calls it `old_blended_noise` and it is not a
/// leftover: `final_density` is built on it, so no terrain exists without it.
///
/// Three stacks of octaves rather than one. Two of them are limits — a floor
/// and a ceiling — and the third chooses between them, so the result is an
/// interpolation whose *blend factor* is itself noise. That is what gives 1.17
/// terrain its overhangs: a smooth field cannot produce them, and a field that
/// picks between two smooth fields can.
///
/// Its octaves are seeded **in sequence** from one generator, not by name.
/// That is the older scheme, and it is why this class cannot reuse
/// PerlinNoise::create: the same amplitudes seeded the two ways give two
/// different worlds.
class BlendedNoise {
public:
    [[nodiscard]] static BlendedNoise create(math::XoroshiroRandomSource& random, f64 xz_scale,
                                             f64 y_scale, f64 xz_factor, f64 y_factor,
                                             f64 smear_scale_multiplier);

    /// The same three stacks drawn from a legacy generator — the Nether's,
    /// which the game seeds as `new LegacyRandomSource(seed)` and nothing else.
    [[nodiscard]] static BlendedNoise create(math::LegacyRandomSource& random, f64 xz_scale,
                                             f64 y_scale, f64 xz_factor, f64 y_factor,
                                             f64 smear_scale_multiplier);

    [[nodiscard]] f64 value(i32 x, i32 y, i32 z) const noexcept;

    /// The selector stack's raw sum, before the blend maps it to [0, 1].
    ///
    /// An instrument, and it answers a question that was asked in
    /// docs/provenance/amplitude-old-blended-noise.md § 5 and never measured:
    /// the blend is `(selector / 10 + 1) / 2`, and eight octaves whose weights
    /// double reach several hundred, so the suspicion was that it saturates
    /// almost everywhere and turns a crossfade into a hard switch. Whether it
    /// does is a fact about this field, not a matter of opinion, and this is
    /// how it is read.
    [[nodiscard]] f64 selector(i32 x, i32 y, i32 z) const noexcept;

    /// What `selector` is divided by before the blend. One, unless
    /// `OV_SELECTOR_DIV` says otherwise.
    [[nodiscard]] f64 selector_divisor() const noexcept { return selector_divisor_; }

    [[nodiscard]] f64 max_value() const noexcept { return max_value_; }

private:
    /// A stack of octaves seeded one after another rather than by name.
    ///
    /// The highest-frequency octave is created *first* and the rest descend, so
    /// the order in the generator's stream is the reverse of the array.
    struct LegacyStack {
        std::vector<std::unique_ptr<ImprovedNoise>> octaves;
        i32                                         first_octave{0};

        template<typename Random>
        [[nodiscard]] static LegacyStack create(Random& random, i32 first_octave, usize count);
    };

    template<typename Random>
    [[nodiscard]] static BlendedNoise build(Random& random, f64 xz_scale, f64 y_scale,
                                            f64 xz_factor, f64 y_factor,
                                            f64 smear_scale_multiplier);

    LegacyStack min_limit_;
    LegacyStack max_limit_;
    LegacyStack main_;

    f64 xz_multiplier_{0.0};
    f64 y_multiplier_{0.0};
    f64 xz_factor_{0.0};
    f64 y_factor_{0.0};
    f64 smear_scale_multiplier_{0.0};
    f64 max_value_{0.0};

    /// A measuring instrument for the selector, read from `OV_SELECTOR_DIV`
    /// once when the noise is built. One means the field as it stands; a large
    /// value is the "the game normalises this stack" hypothesis, and the
    /// Nether's distribution oracle is what decides between them. Never
    /// changed by anything but that environment variable.
    f64 selector_divisor_{1.0};
};

}  // namespace ov::worldgen
