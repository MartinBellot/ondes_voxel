// Monotonic time and the fixed-rate tick clock.
//
// Gameplay logic must never read wall-clock time: it breaks determinism and
// makes parity tests impossible (principle 5). Logic reads the tick counter;
// only the loop that drives it looks at a real clock.
#pragma once

#include "ov/base/types.hpp"

#include <chrono>

namespace ov {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

/// Minecraft runs at 20 ticks per second: one tick is exactly 50 ms.
inline constexpr i64  kTicksPerSecond = 20;
inline constexpr auto kTickDuration   = std::chrono::milliseconds{1000 / kTicksPerSecond};

[[nodiscard]] inline TimePoint now() noexcept {
    return Clock::now();
}

[[nodiscard]] inline f64 to_millis(Duration d) noexcept {
    return std::chrono::duration<f64, std::milli>{d}.count();
}

/// Drives a fixed-rate loop.
///
/// Accumulates elapsed real time and yields whole ticks. The ticks that are due
/// wait in `pending_ticks()`; the caller takes them one at a time with
/// `take_tick()` and runs one whole tick for each, back to back and without
/// sleeping in between — vanilla's catch-up. When the loop falls further behind
/// than `max_catch_up_ticks()`, the rest of the backlog is dropped rather than
/// replayed: running 200 ticks back to back makes a struggling server struggle
/// harder. Vanilla does the same and reports it as "Can't keep up".
///
/// `tick_count()` counts the ticks *taken*, never the ones due or dropped, so
/// game time is simulated time and nothing of the wall clock leaks into it
/// (principle 5). It used to count every tick yielded while the server ran one
/// tick per call, and jumped by the lag.
class TickClock {
public:
    explicit TickClock(i64 ticks_per_second = kTicksPerSecond) noexcept;
    /// A clock whose time starts at `start`: with `advance(TimePoint)`, a
    /// clock a test drives by hand.
    TickClock(i64 ticks_per_second, TimePoint start) noexcept;

    /// Advance by real time elapsed since the previous call. Returns how many
    /// ticks became due now (0 or more; with the ones still pending, never more
    /// than max_catch_up_ticks()). They wait in `pending_ticks()`.
    [[nodiscard]] i32 advance() noexcept;
    [[nodiscard]] i32 advance(TimePoint current) noexcept;

    /// Take one due tick: true, and `tick_count()` counts it, if one was
    /// pending; false, and nothing counted, if none was. One whole tick is
    /// run for each true.
    [[nodiscard]] bool take_tick() noexcept;
    [[nodiscard]] i32  pending_ticks() const noexcept { return pending_; }

    /// True if the previous advance() dropped ticks to avoid a death spiral,
    /// and how many it dropped.
    [[nodiscard]] bool is_behind() const noexcept { return behind_; }
    [[nodiscard]] i64  dropped_ticks() const noexcept { return dropped_; }

    /// Ticks run: the ones taken with take_tick(). What game logic reads.
    [[nodiscard]] i64 tick_count() const noexcept { return tick_count_; }

    /// Fraction of the way into the current tick, in [0, 1). Used by the client
    /// to interpolate rendering between two ticks; never by game logic.
    [[nodiscard]] f32 partial_tick() const noexcept;

    /// How long to sleep before the next tick is due. Zero when one is already
    /// due, pending ones included: catch-up ticks do not wait.
    [[nodiscard]] Duration time_until_next_tick() const noexcept;

    /// Ticks the clock holds due at once before declaring itself behind.
    [[nodiscard]] static constexpr i32 max_catch_up_ticks() noexcept { return 10; }

    /// Forget the backlog, the ticks due and the behind flag (a pause is not
    /// caught up afterwards). The count of ticks run stays.
    void reset() noexcept;

private:
    Duration  tick_duration_;
    TimePoint last_;
    Duration  accumulator_{};
    i64       tick_count_{0};
    i32       pending_{0};
    i64       dropped_{0};
    bool      behind_{false};
};

}  // namespace ov
