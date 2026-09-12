#include "ov/base/time.hpp"

#include "ov/base/assert.hpp"

namespace ov {

TickClock::TickClock(i64 ticks_per_second) noexcept : TickClock{ticks_per_second, now()} {}

TickClock::TickClock(i64 ticks_per_second, TimePoint start) noexcept
    : tick_duration_{std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) /
                     ticks_per_second},
      last_{start} {
    OV_ENSURE(ticks_per_second > 0, "ticks_per_second must be positive, got {}", ticks_per_second);
}

i32 TickClock::advance() noexcept { return advance(now()); }

i32 TickClock::advance(TimePoint current) noexcept {
    // A backwards steady_clock should be impossible; clamp rather than let a
    // negative duration underflow the accumulator.
    const Duration elapsed = current > last_ ? current - last_ : Duration::zero();
    last_                  = current;
    accumulator_ += elapsed;

    i32 ticks = 0;
    while (accumulator_ >= tick_duration_ && pending_ + ticks < max_catch_up_ticks()) {
        accumulator_ -= tick_duration_;
        ++ticks;
    }

    // Still a backlog after the cap: we are not keeping up. Drop it instead of
    // replaying it — running 200 ticks back to back makes a struggling server
    // struggle harder, which is the classic death spiral. Dropped ticks are
    // never counted: they did not run.
    behind_  = accumulator_ >= tick_duration_;
    dropped_ = behind_ ? static_cast<i64>(accumulator_ / tick_duration_) : 0;
    if (behind_) {
        accumulator_ = Duration::zero();
    }

    pending_ += ticks;
    return ticks;
}

bool TickClock::take_tick() noexcept {
    if (pending_ <= 0) {
        return false;
    }
    --pending_;
    ++tick_count_;
    return true;
}

f32 TickClock::partial_tick() const noexcept {
    const auto num = static_cast<f64>(accumulator_.count());
    const auto den = static_cast<f64>(tick_duration_.count());
    return static_cast<f32>(num / den);
}

Duration TickClock::time_until_next_tick() const noexcept {
    if (pending_ > 0 || accumulator_ >= tick_duration_) {
        return Duration::zero();
    }
    return tick_duration_ - accumulator_;
}

void TickClock::reset() noexcept {
    last_        = now();
    accumulator_ = Duration::zero();
    pending_     = 0;
    dropped_     = 0;
    behind_      = false;
}

}  // namespace ov
