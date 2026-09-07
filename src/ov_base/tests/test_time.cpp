#include "ov/base/time.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

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
    REQUIRE(fast.advance() == 0);
}
