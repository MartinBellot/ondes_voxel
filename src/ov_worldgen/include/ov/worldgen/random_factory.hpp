// The world's positional random factory, whichever generator the settings name.
//
// A `noise_settings` file carries one boolean that changes every seeded thing
// in its dimension: `legacy_random_source`. The overworld says false and gets
// Xoroshiro128++ everywhere. The Nether says **true**, and then the factory
// every noise, every surface gradient and every surface depth is seeded from
// is a `java.util.Random` factory instead — one long of state rather than two,
// names hashed with `String.hashCode` rather than MD5, a positional seed XORed
// into one long rather than two.
//
// Before this type existed the router and the surface stage built a Xoroshiro
// factory unconditionally, which is why the Nether was measured only as a
// *distribution* (docs/provenance/amplitude-old-blended-noise.md § 5 names it:
// "notre tirage n'est pas le sien"). With it the Nether can be compared cell by
// cell — see docs/provenance/nether.md.
//
// A small closed variant rather than a virtual interface: there are exactly two
// generators in the game, both known here, and the choice is made once per
// dimension at load.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/noise.hpp"

#include <span>
#include <string_view>
#include <variant>

namespace ov::worldgen {

class PositionalRandomFactory {
public:
    explicit PositionalRandomFactory(math::XoroshiroPositionalFactory factory) noexcept
        : factory_(factory) {}
    explicit PositionalRandomFactory(math::LegacyPositionalFactory factory) noexcept
        : factory_(factory) {}

    /// `settings.getRandomSource().newInstance(seed).forkPositional()`: the
    /// factory a dimension's `RandomState` is built around.
    ///
    /// Xoroshiro forks two longs from `new XoroshiroRandomSource(seed)`; the
    /// legacy source forks one long from `new LegacyRandomSource(seed)`.
    [[nodiscard]] static PositionalRandomFactory for_world(i64 seed, bool legacy) noexcept;

    [[nodiscard]] bool legacy() const noexcept {
        return std::holds_alternative<math::LegacyPositionalFactory>(factory_);
    }

    /// `NormalNoise.create(fromHashOf(name), parameters)` — how every named
    /// noise of a `RandomState` is seeded, the router's and the surface's.
    [[nodiscard]] NormalNoise normal_noise(std::string_view name, i32 first_octave,
                                           std::span<const f64> amplitudes) const;

    /// `fromHashOf(name).forkPositional()` — what `vertical_gradient` draws
    /// from.
    [[nodiscard]] PositionalRandomFactory fork_named(std::string_view name) const noexcept;

    /// `at(x, y, z).nextFloat()`.
    [[nodiscard]] f32 next_float_at(i32 x, i32 y, i32 z) const noexcept;

    /// `at(x, y, z).nextDouble()`.
    [[nodiscard]] f64 next_double_at(i32 x, i32 y, i32 z) const noexcept;

    /// The underlying factory, for the one caller that needs a generator of its
    /// own type — the clay bands. Null when the factory is the other kind.
    [[nodiscard]] const math::XoroshiroPositionalFactory* xoroshiro() const noexcept {
        return std::get_if<math::XoroshiroPositionalFactory>(&factory_);
    }
    [[nodiscard]] const math::LegacyPositionalFactory* legacy_factory() const noexcept {
        return std::get_if<math::LegacyPositionalFactory>(&factory_);
    }

private:
    std::variant<math::XoroshiroPositionalFactory, math::LegacyPositionalFactory> factory_;
};

}  // namespace ov::worldgen
