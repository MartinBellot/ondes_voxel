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

    [[nodiscard]] f64 value(f64 x, f64 y, f64 z) const noexcept;

    [[nodiscard]] f64 max_value() const noexcept { return max_value_; }

private:
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

}  // namespace ov::worldgen
