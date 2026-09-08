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

// ── Xoroshiro128++ ──────────────────────────────────────────────────────────

/// The 64-bit golden ratio, used to space seeds apart before mixing.
inline constexpr u64 kGoldenRatio64 = 0x9E3779B97F4A7C15ULL;

/// The 64-bit silver ratio, XORed into a seed before it is upgraded to 128
/// bits. The JDK's own Xoroshiro128PlusPlus uses the same constant the same
/// way, which is what makes it a usable oracle for this whole path.
inline constexpr u64 kSilverRatio64 = 0x6A09E667F3BCC909ULL;

/// Stafford's variant 13 mix — the finaliser SplitMix64 uses.
///
/// Its whole job is to take two seeds that differ in one bit and give back two
/// states that share nothing. Skipping it makes neighbouring world seeds
/// produce visibly similar terrain.
[[nodiscard]] u64 mix_stafford_13(u64 z) noexcept;

/// Xoroshiro128++, the generator the game moved to in 1.18.
///
/// Worldgen runs on this one, so its stream is the terrain. The transition
/// function and the seed mix are both published algorithms (Blackman & Vigna;
/// Stafford), and both are checked against the JDK's own implementation — see
/// the tests.
///
/// Note how much differs from the legacy source beyond the core: the bounded
/// draw uses Lemire's multiply-and-reject rather than a modulo rejection, and
/// the float and double draws take their bits from the *top* of a 64-bit
/// result rather than assembling two smaller draws. Reusing the legacy versions
/// here would consume the wrong amount of state and silently reshape the world.
class XoroshiroRandomSource {
public:
    /// Seeds the way the game does: XOR by the silver ratio, space the second
    /// half by the golden ratio, then mix both. A single 64-bit world seed
    /// becomes 128 bits of state, and seed 0 lands nowhere near the all-zero
    /// state Xoroshiro can never leave.
    explicit XoroshiroRandomSource(i64 seed) noexcept;

    /// Build directly from a 128-bit state, for derived generators and tests.
    XoroshiroRandomSource(u64 lo, u64 hi) noexcept : lo_{lo}, hi_{hi} {}

    [[nodiscard]] i64 next_long() noexcept;

    /// The low 32 bits of a 64-bit draw. One draw, not two.
    [[nodiscard]] i32 next_int() noexcept { return static_cast<i32>(next_long()); }

    /// Uniform in `[0, bound)` by Lemire's method: multiply into the high half
    /// and reject only the short tail. Not the legacy modulo rejection, and not
    /// interchangeable with it — the two consume different amounts of state.
    [[nodiscard]] i32 next_int(i32 bound) noexcept;

    [[nodiscard]] bool next_boolean() noexcept { return (next_long() & 1) != 0; }

    /// The top 24 bits of one draw, scaled by 2^-24.
    [[nodiscard]] f32 next_float() noexcept;

    /// The top 53 bits of one draw, scaled by 2^-53.
    [[nodiscard]] f64 next_double() noexcept;

    /// Standard normal. Carries the same fdlibm caveat as the legacy source.
    [[nodiscard]] f64 next_gaussian() noexcept;

    [[nodiscard]] u64 state_lo() const noexcept { return lo_; }

    [[nodiscard]] u64 state_hi() const noexcept { return hi_; }

private:
    u64  lo_{0};
    u64  hi_{0};
    f64  next_gaussian_{0.0};
    bool have_next_gaussian_{false};
};

}  // namespace ov::math
