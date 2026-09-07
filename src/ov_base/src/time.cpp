#include "ov/base/time.hpp"

#include "ov/base/assert.hpp"

namespace ov {

TickClock::TickClock(i64 ticks_per_second) noexcept
    : tick_duration_{std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) /
                     ticks_per_second},
      last_{now()} {
    OV_ENSURE(ticks_per_second > 0, "ticks_per_second must be positive, got {}", ticks_per_second);
}

i32 TickClock::advance() noexcept {
    const TimePoint current = now();
    // A backwards steady_clock should be impossible; clamp rather than let a
    // negative duration underflow the accumulator.
    const Duration elapsed = current > last_ ? current - last_ : Duration::zero();
    last_ = current;
    accumulator_ += elapsed;

    i32 ticks = 0;
    while (accumulator_ >= tick_duration_ && ticks < max_catch_up_ticks()) {
        accumulator_ -= tick_duration_;
        ++ticks;
    }

    // Still a backlog after the cap: we are not keeping up. Drop it instead of
    // replaying it — running 200 ticks back to back makes a struggling server
    // struggle harder, which is the classic death spiral.
    behind_ = accumulator_ >= tick_duration_;
    if (behind_) {
        accumulator_ = Duration::zero();
    }

    tick_count_ += ticks;
    return ticks;
}

f32 TickClock::partial_tick() const noexcept {
    const auto num = static_cast<f64>(accumulator_.count());
    const auto den = static_cast<f64>(tick_duration_.count());
    return static_cast<f32>(num / den);
}

Duration TickClock::time_until_next_tick() const noexcept {
    return accumulator_ >= tick_duration_ ? Duration::zero() : tick_duration_ - accumulator_;
}

void TickClock::reset() noexcept {
    last_        = now();
    accumulator_ = Duration::zero();
    behind_      = false;
}

}  // namespace ov
