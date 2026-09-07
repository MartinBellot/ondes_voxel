#include "ov/base/alloc_scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace ov;

#if !defined(NDEBUG)

namespace {

/// Guard that restores the abort setting, so one test cannot leave the process
/// configured to ignore violations for the next.
struct ReportOnly {
    ReportOnly() {
        set_abort_on_violation(false);
        reset_allocation_stats();
    }

    /// Under a sanitizer, operator new is ASan's and nothing is counted. The
    /// tests then assert nothing rather than assert something false.
    [[nodiscard]] static bool tracking() { return allocation_tracking_active(); }

    ~ReportOnly() { set_abort_on_violation(true); }
};

}  // namespace

TEST_CASE("allocations are counted", "[base][alloc]") {
    const ReportOnly guard;

    const u64 before = allocation_stats().allocations;
    {
        std::vector<int> v;
        v.reserve(1000);
    }
    if (guard.tracking()) {
        REQUIRE(allocation_stats().allocations > before);
    }
}

TEST_CASE("no violation outside a scope", "[base][alloc]") {
    const ReportOnly guard;

    std::vector<int> v(500);
    REQUIRE(allocation_stats().violations == 0);
}

TEST_CASE("an allocation inside a scope is a violation", "[base][alloc]") {
    // The whole point: this is the bug the tick loop must never have, and it
    // has to be detected rather than reviewed for.
    const ReportOnly guard;

    {
        const NoAllocScope scope{"test"};
        std::vector<int>   v(500);
        REQUIRE(v.size() == 500);
    }
    if (guard.tracking()) {
        REQUIRE(allocation_stats().violations >= 1);
    }
}

TEST_CASE("a scope with no allocation is clean", "[base][alloc]") {
    const ReportOnly guard;

    // Realistic shape of tick code: a preallocated buffer written in place.
    std::vector<int> buffer(1000, 0);
    {
        const NoAllocScope scope{"tick"};
        for (usize i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<int>(i * 2);
        }
        REQUIRE(buffer[10] == 20);
    }
    REQUIRE(allocation_stats().violations == 0);
}

TEST_CASE("a vector past its reserve is caught", "[base][alloc]") {
    // The classic one. It looks like nothing, and it reallocates.
    const ReportOnly guard;

    std::vector<int> v;
    v.reserve(4);
    {
        const NoAllocScope scope{"tick"};
        for (int i = 0; i < 4; ++i) {
            v.push_back(i);  // within the reserve
        }
        REQUIRE(allocation_stats().violations == 0);

        v.push_back(4);  // past it: reallocates
    }
    if (guard.tracking()) {
        REQUIRE(allocation_stats().violations >= 1);
    }
}

TEST_CASE("a string built for a message is caught", "[base][alloc]") {
    // The other classic: a log line assembled inside the tick. Short strings
    // live in the small-string buffer, so the test uses one that cannot.
    const ReportOnly guard;

    {
        const NoAllocScope scope{"tick"};
        std::string        message(200, 'x');
        REQUIRE(message.size() == 200);
    }
    if (guard.tracking()) {
        REQUIRE(allocation_stats().violations >= 1);
    }
}

TEST_CASE("scopes nest", "[base][alloc]") {
    const ReportOnly guard;

    {
        const NoAllocScope outer{"outer"};
        {
            const NoAllocScope inner{"inner"};
        }
        // Leaving the inner scope must not lift the outer one.
        std::vector<int> v(500);
        REQUIRE(v.size() == 500);
    }
    if (guard.tracking()) {
        REQUIRE(allocation_stats().violations >= 1);
    }
}

TEST_CASE("the scope is per thread", "[base][alloc]") {
    // The tick thread declares the invariant; the worker pool allocates freely
    // at the same moment. A process-wide flag would make the check unusable.
    //
    // The thread is started *before* entering the scope, and the handshake is a
    // spin on two atomics: constructing a std::thread allocates, and joining
    // may too, both on the calling thread. The first version of this test did
    // it inside the scope and was reported as three violations — which is the
    // check working, not failing.
    const ReportOnly guard;

    std::atomic<bool> start{false};
    std::atomic<bool> finished{false};

    std::thread worker{[&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        std::vector<int> v(500);
        (void)v;
        finished.store(true, std::memory_order_release);
    }};

    {
        const NoAllocScope scope{"tick thread"};
        const u64          before = allocation_stats().violations;

        start.store(true, std::memory_order_release);
        while (!finished.load(std::memory_order_acquire)) {
        }

        // The worker allocated while this thread was inside the scope, and it
        // is not this thread's problem.
        REQUIRE(allocation_stats().violations == before);
    }

    worker.join();
}

TEST_CASE("statistics reset", "[base][alloc]") {
    const ReportOnly guard;

    {
        const NoAllocScope scope{"test"};
        std::vector<int>   v(100);
    }
    if (guard.tracking()) {
        REQUIRE(allocation_stats().violations >= 1);
    }

    reset_allocation_stats();
    REQUIRE(allocation_stats().violations == 0);
    REQUIRE(allocation_stats().allocations == 0);
}

#else

TEST_CASE("tracking compiles away in release", "[base][alloc]") {
    STATIC_REQUIRE_FALSE(kAllocationTrackingEnabled);

    // The guard must still be constructible, so tick code needs no #ifdef.
    const NoAllocScope scope{"release"};
    std::vector<int>   v(100);
    REQUIRE(v.size() == 100);
    REQUIRE(allocation_stats().allocations == 0);
}

#endif
