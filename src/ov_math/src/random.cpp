#include "ov/math/random.hpp"

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

}  // namespace ov::math
