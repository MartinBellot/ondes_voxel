#include "ov/base/time.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

using namespace ov;

TEST_CASE("a tick is exactly 50 ms", "[time]") {
    // 20 TPS is not a target, it is the definition of a Minecraft tick. Every
    // gameplay duration in the codebase is expressed in ticks and converted
    // through this constant.
    REQUIRE(kTicksPerSecond == 20);
    REQUIRE(kTickDuration == std::chrono::milliseconds{50});
}

TEST_CASE("TickClock yields no tick before one is due", "[time]") {
    TickClock clock;
    REQUIRE(clock.advance() == 0);
    REQUIRE(clock.tick_count() == 0);
    REQUIRE_FALSE(clock.is_behind());
}

TEST_CASE("TickClock yields ticks as real time elapses", "[time]") {
    TickClock clock;
    std::this_thread::sleep_for(std::chrono::milliseconds{120});

    const i32 ticks = clock.advance();
    // 120 ms is 2.4 ticks: two whole ticks, with the remainder carried over.
    // Sleep overshoot on a loaded machine can push this higher, so the check is
    // a range rather than an equality.
    REQUIRE(ticks >= 2);
    REQUIRE(ticks <= TickClock::max_catch_up_ticks());
    // Due is not run: the count moves only as the ticks are taken.
    REQUIRE(clock.pending_ticks() == ticks);
    REQUIRE(clock.tick_count() == 0);
    while (clock.take_tick()) {
    }
    REQUIRE(clock.tick_count() == ticks);
}

TEST_CASE("TickClock caps catch-up instead of replaying a backlog", "[time]") {
    // A clock constructed at 10000 TPS accumulates an enormous backlog from a
    // short sleep. Replaying it would be the classic death spiral: the server
    // falls behind, runs hundreds of ticks to catch up, and falls further
    // behind. The backlog is dropped and the clock reports that it happened.
    TickClock fast{10000};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const i32 ticks = fast.advance();
    REQUIRE(ticks == TickClock::max_catch_up_ticks());
    REQUIRE(fast.is_behind());

    // Having dropped the backlog, the very next advance must start clean.
    const i32 next = fast.advance();
    REQUIRE(next <= TickClock::max_catch_up_ticks());
}

TEST_CASE("partial_tick stays in [0, 1)", "[time]") {
    TickClock clock;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    (void)clock.advance();

    const f32 partial = clock.partial_tick();
    REQUIRE(partial >= 0.0f);
    REQUIRE(partial < 1.0f);
}

TEST_CASE("time_until_next_tick shrinks as the tick approaches", "[time]") {
    TickClock clock;
    (void)clock.advance();
    const Duration first = clock.time_until_next_tick();
    REQUIRE(first <= kTickDuration);

    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    (void)clock.advance();
    REQUIRE(clock.time_until_next_tick() <= first);
}

TEST_CASE("reset clears the accumulator and the behind flag", "[time]") {
    TickClock fast{10000};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    (void)fast.advance();
    REQUIRE(fast.is_behind());

    fast.reset();
    REQUIRE_FALSE(fast.is_behind());
    REQUIRE(fast.pending_ticks() == 0);  // a pause is not caught up afterwards
    REQUIRE(fast.advance() == 0);
}

// ── tick accounting ── Driven by hand: the loop the server runs, one whole
// tick per take_tick(), against a clock whose time the test sets.

TEST_CASE("TickClock: three ticks due run three bodies, back to back, and count three",
          "[time]") {
    const TimePoint start{};
    TickClock       clock{20, start};
    REQUIRE(clock.advance(start + std::chrono::milliseconds{150}) == 3);
    REQUIRE(clock.tick_count() == 0);                           // due is not run
    REQUIRE(clock.time_until_next_tick() == Duration::zero());  // no wait between them

    std::vector<i64> seen;  // the tick each body sees
    while (clock.take_tick()) {
        seen.push_back(clock.tick_count());
    }
    REQUIRE(seen == std::vector<i64>{1, 2, 3});
    REQUIRE(clock.tick_count() == 3);
    REQUIRE_FALSE(clock.is_behind());
    REQUIRE_FALSE(clock.take_tick());  // no body without a tick due
}

TEST_CASE("TickClock: past the cap the surplus is dropped, and only what ran is counted",
          "[time]") {
    const TimePoint start{};
    TickClock       clock{20, start};
    // Twenty-five ticks of lag: ten run (the cap), fifteen are dropped.
    REQUIRE(clock.advance(start + std::chrono::milliseconds{25 * 50}) ==
            TickClock::max_catch_up_ticks());
    REQUIRE(clock.is_behind());
    REQUIRE(clock.dropped_ticks() == 15);

    i32 bodies = 0;
    while (clock.take_tick()) {
        ++bodies;
    }
    REQUIRE(bodies == TickClock::max_catch_up_ticks());
    REQUIRE(clock.tick_count() == TickClock::max_catch_up_ticks());

    // The next tick starts clean: one more tick of time, one more tick.
    REQUIRE(clock.advance(start + std::chrono::milliseconds{26 * 50}) == 1);
    REQUIRE_FALSE(clock.is_behind());
    REQUIRE(clock.dropped_ticks() == 0);
    REQUIRE(clock.take_tick());
    REQUIRE(clock.tick_count() == TickClock::max_catch_up_ticks() + 1);
}

TEST_CASE("TickClock: ticks already pending count against the cap", "[time]") {
    const TimePoint start{};
    TickClock       clock{20, start};
    REQUIRE(clock.advance(start + std::chrono::milliseconds{8 * 50}) == 8);
    // Eight still pending: only two more fit under the cap of ten.
    REQUIRE(clock.advance(start + std::chrono::milliseconds{13 * 50}) == 2);
    REQUIRE(clock.pending_ticks() == TickClock::max_catch_up_ticks());
    REQUIRE(clock.is_behind());
    REQUIRE(clock.dropped_ticks() == 3);
}
