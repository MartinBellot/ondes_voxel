// The random sources the game draws from, reproduced bit for bit.
//
// This is the quietest way to get the world wrong. A generator that is merely
// *a* good generator produces terrain that looks entirely plausible and shares
// not one block with vanilla, and nothing reports an error. The same is true of
// one extra call, or one call in the wrong order. There is no symptom until
// someone compares two screenshots.
//
// So the implementations here are pinned to reference vectors rather than to
// their own idea of correctness, and the vectors come from outside this
// project — see tests/test_random.cpp.
//
// `LegacyRandomSource` is `java.util.Random`, whose algorithm the JDK
// specifies exactly, down to the constants. That specification is the source
// used here; it is why this one can be checked against any JVM on any machine.
#pragma once

#include "ov/base/types.hpp"

namespace ov::math {

/// The multiplier, addend and mask of the linear congruential generator that
/// `java.util.Random` — and therefore Minecraft's legacy source — is defined
/// by. Spelled out because they are the specification, not an implementation
/// choice.
inline constexpr u64 kLegacyMultiplier = 0x5DEECE66DULL;
inline constexpr u64 kLegacyAddend     = 0xBULL;
inline constexpr u64 kLegacyMask       = (1ULL << 48) - 1;

/// `java.util.Random`: a 48-bit LCG, seeded through an XOR scramble.
///
/// Used throughout the game for everything predating 1.18 — mob behaviour,
/// loot, item drops, block ticks — and still for a great deal after it.
///
/// Not thread-safe and deliberately not synchronised. Java's is synchronised;
/// sharing one of these across threads would make the world non-deterministic,
/// which is the one thing this type exists to prevent, so the fix is never to
/// share it.
class LegacyRandomSource {
public:
    /// Seeds exactly as `new Random(seed)` does, XOR scramble included.
    explicit LegacyRandomSource(i64 seed) noexcept { set_seed(seed); }

    void set_seed(i64 seed) noexcept;

    /// The generator's primitive: advance, then take the top `bits` bits.
    /// Every other method is defined in terms of this one, in a specific order
    /// that is itself part of the specification.
    [[nodiscard]] i32 next(int bits) noexcept;

    [[nodiscard]] i32 next_int() noexcept { return next(32); }

    [[nodiscard]] i64 next_long() noexcept;

    [[nodiscard]] bool next_boolean() noexcept { return next(1) != 0; }

    [[nodiscard]] f32 next_float() noexcept;
    [[nodiscard]] f64 next_double() noexcept;

    /// Uniform in `[0, bound)`. Rejection sampling, not a modulo.
    ///
    /// A plain `next_int() % bound` is biased towards low values, and the bias
    /// is small enough to look like noise while shifting every ore vein and
    /// loot roll in the game. Bounds that are powers of two take a separate
    /// path, as the specification requires.
    ///
    /// A bound of zero or less returns 0 rather than trapping: this is reached
    /// from data, and a malformed datapack must not be able to abort a tick.
    [[nodiscard]] i32 next_int(i32 bound) noexcept;

    /// Standard normal, by the polar method with a cached second value.
    ///
    /// ⚠️ The one method here that is not guaranteed bit-exact against a JVM:
    /// it calls `log`, and Java specifies fdlibm semantics while the platform's
    /// libm is free to differ in the last place. See the tests, which state
    /// exactly how far the agreement was measured to go.
    [[nodiscard]] f64 next_gaussian() noexcept;

    /// The raw internal state, for tests and for seeding derived generators.
    [[nodiscard]] u64 raw_seed() const noexcept { return seed_; }

private:
    u64  seed_{0};
    f64  next_gaussian_{0.0};
    bool have_next_gaussian_{false};
};

}  // namespace ov::math
