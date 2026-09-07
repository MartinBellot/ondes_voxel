// Monotonic time and the fixed-rate tick clock.
//
// Gameplay logic must never read wall-clock time: it breaks determinism and
// makes parity tests impossible (principle 5). Logic reads the tick counter;
// only the loop that drives it looks at a real clock.
#pragma once

#include <chrono>

#include "ov/base/types.hpp"

namespace ov {

using Clock    = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

/// Minecraft runs at 20 ticks per second: one tick is exactly 50 ms.
inline constexpr i64 kTicksPerSecond = 20;
inline constexpr auto kTickDuration  = std::chrono::milliseconds{1000 / kTicksPerSecond};

[[nodiscard]] inline TimePoint now() noexcept { return Clock::now(); }

[[nodiscard]] inline f64 to_millis(Duration d) noexcept {
    return std::chrono::duration<f64, std::milli>{d}.count();
}

/// Drives a fixed-rate loop.
///
/// Accumulates elapsed real time and yields whole ticks. When the loop falls
/// behind, the backlog is capped rather than replayed: catching up by running
/// 200 ticks back to back makes a struggling server struggle harder. Vanilla
/// does the same and reports it as "Can't keep up".
class TickClock {
public:
    explicit TickClock(i64 ticks_per_second = kTicksPerSecond) noexcept;

    /// Advance by real time elapsed since the previous call. Returns how many
    /// ticks should run now (0 or more, capped by max_catch_up_ticks()).
    [[nodiscard]] i32 advance() noexcept;

    /// True if the previous advance() dropped ticks to avoid a death spiral.
    [[nodiscard]] bool is_behind() const noexcept { return behind_; }

    /// Total ticks yielded since construction.
    [[nodiscard]] i64 tick_count() const noexcept { return tick_count_; }

    /// Fraction of the way into the current tick, in [0, 1). Used by the client
    /// to interpolate rendering between two ticks; never by game logic.
    [[nodiscard]] f32 partial_tick() const noexcept;

    /// How long to sleep before the next tick is due. Zero when already due.
    [[nodiscard]] Duration time_until_next_tick() const noexcept;

    /// Ticks the clock will run in one advance() before declaring itself behind.
    [[nodiscard]] static constexpr i32 max_catch_up_ticks() noexcept { return 10; }

    void reset() noexcept;

private:
    Duration  tick_duration_;
    TimePoint last_;
    Duration  accumulator_{};
    i64       tick_count_{0};
    bool      behind_{false};
};

}  // namespace ov
