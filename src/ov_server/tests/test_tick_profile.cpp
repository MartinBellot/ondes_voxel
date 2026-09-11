// The tick profile: a histogram whose error is bounded and stated, and a phase
// clock that names the phase a slow tick spent its time in.
#include "../src/tick_profile.hpp"

#include "ov/base/alloc_scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <thread>

using namespace ov;
using namespace ov::server;

TEST_CASE("a histogram bucket is never more than 1/32 below its samples", "[tick_profile]") {
    usize previous = 0;
    for (i64 value = 0; value < 5'000'000; value += (value < 1000 ? 1 : value / 97)) {
        const usize bucket = LatencyHistogram::bucket_of(value);
        const i64   floor  = LatencyHistogram::bucket_floor(bucket);
        CAPTURE(value, bucket, floor);
        REQUIRE(bucket >= previous);  // monotonic
        REQUIRE(floor <= value);
        if (value < static_cast<i64>(LatencyHistogram::kExactBelow)) {
            REQUIRE(floor == value);
        } else {
            REQUIRE(value - floor <= value / 32);
        }
        previous = bucket;
    }
    // The last bucket absorbs anything absurd rather than indexing past it.
    CHECK(LatencyHistogram::bucket_of(std::numeric_limits<i64>::max()) ==
          LatencyHistogram::kBucketCount - 1);
    CHECK(LatencyHistogram::bucket_of(-5) == 0);
}

TEST_CASE("counts, totals and the budget are exact; percentiles are within 1/32",
          "[tick_profile]") {
    LatencyHistogram h;
    CHECK(h.percentile(0.5) == 0);  // empty reads zero, not garbage

    for (i64 v = 1; v <= 100'000; ++v) {
        h.record(v);
    }
    CHECK(h.count() == 100'000);
    CHECK(h.max() == 100'000);
    CHECK(h.total() == 100'000LL * 100'001 / 2);
    CHECK(h.over_budget() == 50'000);  // strictly above 50 ms
    CHECK(h.mean() == 50'000);

    // The rank rule of the server's other histograms: quantile * (n - 1).
    const auto near = [](i64 got, i64 want) {
        return got <= want && want - got <= want / 32 + 1;
    };
    CHECK(near(h.percentile(0.50), 50'000));
    CHECK(near(h.percentile(0.90), 90'000));
    CHECK(near(h.percentile(0.99), 99'000));
    CHECK(h.percentile(1.0) == 100'000);  // never above the exact maximum
}

TEST_CASE("the phase a slow tick slept in is the one named for it", "[tick_profile]") {
    TickProfile profile;
    profile.begin_tick();
    CHECK(profile.enter(TickPhase::Commands) == TickPhase::Other);
    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    CHECK(profile.enter(TickPhase::Autosave) == TickPhase::Commands);
    profile.end_tick();

    CHECK(profile.iterations() == 1);
    CHECK(profile.slow_ticks() == 1);
    CHECK(profile.heaviest_in_slow(TickPhase::Commands) == 1);
    CHECK(profile.heaviest_in_slow(TickPhase::Autosave) == 0);
    CHECK(profile.phase(TickPhase::Commands).count() == 1);
    CHECK(profile.phase(TickPhase::Commands).max() >= 60'000);
    CHECK(profile.phase(TickPhase::Autosave).count() == 1);
    // A phase that never ran records nothing, not a zero.
    CHECK(profile.phase(TickPhase::Relight).count() == 0);
    // Sleeping is waiting: the wall clock saw 60 ms, the thread's CPU clock
    // almost none. That gap is what tells contention from cost.
    CHECK(profile.cpu_micros(TickPhase::Commands) < 30'000);

    const auto lines = profile.report(1);
    REQUIRE(!lines.empty());
    CHECK(std::ranges::any_of(lines, [](const std::string& line) {
        return line.find("tick phase commands") != std::string::npos &&
               line.find("heaviest in 1 of 1 slow ticks") != std::string::npos;
    }));
}

TEST_CASE("an iteration left open is discarded, not half counted", "[tick_profile]") {
    TickProfile profile;
    profile.begin_tick();
    (void)profile.enter(TickPhase::Entities);
    profile.begin_tick();  // a `continue` skipped the end of the last one
    profile.end_tick();
    profile.end_tick();    // a second end without a begin does nothing
    CHECK(profile.iterations() == 1);
    CHECK(profile.phase(TickPhase::Entities).count() == 0);
}

TEST_CASE("the profile allocates nothing inside the tick", "[tick_profile]") {
    // A preprocessor guard, not `if constexpr`: outside a template the
    // discarded branch is still compiled, and the two hooks it calls do not
    // exist at all in a release build.
#if defined(NDEBUG)
    SKIP("allocation tracking is compiled out of this build");
#else
    if (!allocation_tracking_active()) {
        SKIP("operator new is not replaced in this build");
    }
    TickProfile profile;
    set_abort_on_violation(false);
    const u64 before = allocation_stats().violations;
    {
        const NoAllocScope guard{"tick profile test"};
        for (int i = 0; i < 100; ++i) {
            profile.begin_tick();
            (void)profile.enter(TickPhase::ScheduledTicks);
            (void)profile.enter(TickPhase::Relight);
            profile.block_edit.record(i);
            profile.end_tick();
        }
    }
    const u64 after = allocation_stats().violations;
    set_abort_on_violation(true);
    CHECK(after == before);
#endif
}
