#include "ov/math/random.hpp"

#include "ov/base/md5.hpp"

#include <bit>
#include <cmath>

namespace ov::math {

void LegacyRandomSource::set_seed(i64 seed) noexcept {
    // The XOR scramble is part of the specification, not an optimisation.
    // Without it, `new Random(0)` and `new Random(1)` produce visibly related
    // streams, and every seed in the game would be shifted.
    seed_               = (static_cast<u64>(seed) ^ kLegacyMultiplier) & kLegacyMask;
    have_next_gaussian_ = false;
}

i32 LegacyRandomSource::next(int bits) noexcept {
    seed_ = (seed_ * kLegacyMultiplier + kLegacyAddend) & kLegacyMask;
    // Java's `>>>` on the 48-bit state, then a narrowing cast to int. The cast
    // is what makes half the results negative, and code that "fixes" that by
    // taking an absolute value diverges immediately.
    return static_cast<i32>(static_cast<u32>(seed_ >> (48 - bits)));
}

i64 LegacyRandomSource::next_long() noexcept {
    // Two draws, high word first. The order is observable.
    const i64 high = static_cast<i64>(next(32));
    const i64 low  = static_cast<i64>(next(32));
    return static_cast<i64>((static_cast<u64>(high) << 32) + static_cast<u64>(low));
}

f32 LegacyRandomSource::next_float() noexcept {
    // 24 bits — a float's significand. Dividing by 2^24 lands exactly on
    // representable values, so there is no rounding to disagree about.
    return static_cast<f32>(next(24)) / static_cast<f32>(1 << 24);
}

f64 LegacyRandomSource::next_double() noexcept {
    // 26 bits then 27, in that order, assembled into 53 — a double's
    // significand. Drawing 32 and 32 instead would be simpler, consume the
    // same amount of state, and be wrong.
    const i64 high = static_cast<i64>(next(26)) << 27;
    const i64 low  = static_cast<i64>(next(27));
    return static_cast<f64>(high + low) * 0x1.0p-53;
}

i32 LegacyRandomSource::next_int(i32 bound) noexcept {
    if (bound <= 0) {
        return 0;
    }

    i32       r = next(31);
    const i32 m = bound - 1;

    if ((bound & m) == 0) {
        // A power of two: take the high bits of a 31-bit draw rather than the
        // low ones. The low bits of an LCG are the weakest part of its state.
        return static_cast<i32>((static_cast<i64>(bound) * static_cast<i64>(r)) >> 31);
    }

    // Reject the tail that would make the range uneven. The loop condition is
    // written with unsigned arithmetic because the specification's version
    // relies on signed overflow, which is undefined behaviour here — the
    // wrapping result is the same, and it is the result that is specified.
    for (i32 u = r;; u = next(31)) {
        r               = u % bound;
        const u32 check = static_cast<u32>(u) - static_cast<u32>(r) + static_cast<u32>(m);
        if (static_cast<i32>(check) >= 0) {
            break;
        }
    }
    return r;
}

f64 LegacyRandomSource::next_gaussian() noexcept {
    // The polar method produces two values at a time and Java keeps the spare,
    // so the number of underlying draws depends on how many times this has
    // been called before. Discarding the spare would silently double the state
    // consumed and desynchronise everything downstream.
    if (have_next_gaussian_) {
        have_next_gaussian_ = false;
        return next_gaussian_;
    }

    f64 v1 = 0.0;
    f64 v2 = 0.0;
    f64 s  = 0.0;
    do {
        v1 = 2.0 * next_double() - 1.0;
        v2 = 2.0 * next_double() - 1.0;
        s  = v1 * v1 + v2 * v2;
    } while (s >= 1.0 || s == 0.0);

    const f64 multiplier = std::sqrt(-2.0 * std::log(s) / s);
    next_gaussian_       = v2 * multiplier;
    have_next_gaussian_  = true;
    return v1 * multiplier;
}

// ── Xoroshiro128++ ──────────────────────────────────────────────────────────

u64 mix_stafford_13(u64 z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

XoroshiroRandomSource::XoroshiroRandomSource(i64 seed) noexcept {
    // Two states derived from one seed, spaced by the golden ratio so that
    // adjacent world seeds do not start near each other, then mixed so they
    // share no structure at all.
    const u64 low  = static_cast<u64>(seed) ^ kSilverRatio64;
    const u64 high = low + kGoldenRatio64;
    lo_            = mix_stafford_13(low);
    hi_            = mix_stafford_13(high);
}

i64 XoroshiroRandomSource::next_long() noexcept {
    const u64 s0     = lo_;
    u64       s1     = hi_;
    const u64 result = std::rotl(s0 + s1, 17) + s0;

    s1 ^= s0;
    lo_ = std::rotl(s0, 49) ^ s1 ^ (s1 << 21);
    hi_ = std::rotl(s1, 28);

    return static_cast<i64>(result);
}

i32 XoroshiroRandomSource::next_int(i32 bound) noexcept {
    if (bound <= 0) {
        return 0;
    }

    // Lemire: multiply a 32-bit draw by the bound into 64 bits and keep the
    // high half. The low half says how far into the current bucket the draw
    // landed, which is what makes the rejection test cheap — most calls never
    // draw twice.
    u64 product = static_cast<u64>(static_cast<u32>(next_int())) * static_cast<u64>(bound);
    u64 low     = product & 0xFFFFFFFFULL;

    if (low < static_cast<u64>(bound)) {
        // Only the first `2^32 mod bound` values are over-represented, so only
        // they need rejecting.
        const u32 threshold =
            static_cast<u32>((~static_cast<u32>(bound) + 1U) % static_cast<u32>(bound));
        while (low < threshold) {
            product = static_cast<u64>(static_cast<u32>(next_int())) * static_cast<u64>(bound);
            low     = product & 0xFFFFFFFFULL;
        }
    }

    return static_cast<i32>(product >> 32);
}

f32 XoroshiroRandomSource::next_float() noexcept {
    // The top bits, not the bottom ones, and from a single draw.
    return static_cast<f32>(static_cast<u64>(next_long()) >> 40) * 0x1.0p-24f;
}

f64 XoroshiroRandomSource::next_double() noexcept {
    return static_cast<f64>(static_cast<u64>(next_long()) >> 11) * 0x1.0p-53;
}

f64 XoroshiroRandomSource::next_gaussian() noexcept {
    if (have_next_gaussian_) {
        have_next_gaussian_ = false;
        return next_gaussian_;
    }

    f64 v1 = 0.0;
    f64 v2 = 0.0;
    f64 s  = 0.0;
    do {
        v1 = 2.0 * next_double() - 1.0;
        v2 = 2.0 * next_double() - 1.0;
        s  = v1 * v1 + v2 * v2;
    } while (s >= 1.0 || s == 0.0);

    const f64 multiplier = std::sqrt(-2.0 * std::log(s) / s);
    next_gaussian_       = v2 * multiplier;
    have_next_gaussian_  = true;
    return v1 * multiplier;
}

}  // namespace ov::math

namespace ov::math {

i64 position_seed(i32 x, i32 y, i32 z) noexcept {
    // The first term wraps at 32 bits before it is widened; the second does
    // not. Writing both in 64 bits is the mistake this line exists to avoid,
    // and it only shows past about 686 blocks of x.
    const i64 wrapped_x = static_cast<i64>(static_cast<i32>(
        static_cast<u32>(x) * static_cast<u32>(3129871)));
    i64 value = wrapped_x ^ (static_cast<i64>(z) * 116129781LL) ^ static_cast<i64>(y);
    // Unsigned for the arithmetic, because signed overflow is undefined and
    // this is deliberately allowed to wrap.
    auto bits = static_cast<u64>(value);
    bits      = bits * bits * 42317861ULL + bits * 11ULL;
    value     = static_cast<i64>(bits);
    return value >> 16;
}

XoroshiroPositionalFactory XoroshiroRandomSource::fork_positional() noexcept {
    const auto lo = static_cast<u64>(next_long());
    const auto hi = static_cast<u64>(next_long());
    return XoroshiroPositionalFactory{lo, hi};
}

XoroshiroRandomSource XoroshiroPositionalFactory::at(i32 x, i32 y, i32 z) const noexcept {
    const auto seed = static_cast<u64>(position_seed(x, y, z));
    return XoroshiroRandomSource{seed ^ lo_, hi_};
}

XoroshiroRandomSource XoroshiroPositionalFactory::from_hash_of(
    std::string_view name) const noexcept {
    const auto digest = md5(name);
    // Big-endian, both halves: this is Guava's Longs.fromBytes over the MD5,
    // and reading it the other way round gives a different world rather than
    // an error.
    u64 first  = 0;
    u64 second = 0;
    for (usize i = 0; i < 8; ++i) {
        first  = (first << 8) | digest[i];
        second = (second << 8) | digest[i + 8];
    }
    return XoroshiroRandomSource{first ^ lo_, second ^ hi_};
}

i32 java_string_hash(std::string_view text) noexcept {
    // Over UTF-16 code units in Java. For the ASCII names worldgen uses, the
    // bytes and the code units are the same; anything else would need real
    // decoding, and nothing here has one.
    u32 hash = 0;
    for (const char c : text) {
        hash = hash * 31U + static_cast<u32>(static_cast<u8>(c));
    }
    return static_cast<i32>(hash);
}

LegacyRandomSource LegacyPositionalFactory::at(i32 x, i32 y, i32 z) const noexcept {
    return LegacyRandomSource{static_cast<i64>(static_cast<u64>(position_seed(x, y, z)) ^ seed_)};
}

LegacyRandomSource LegacyPositionalFactory::from_hash_of(std::string_view name) const noexcept {
    return LegacyRandomSource{
        static_cast<i64>(static_cast<u64>(java_string_hash(name)) ^ seed_)};
}

}  // namespace ov::math
