#include "ov/base/job_pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

using namespace ov;

TEST_CASE("a pool with no workers runs jobs inline", "[job_pool]") {
    base::JobPool pool{0};
    REQUIRE(pool.worker_count() == 0);

    std::vector<usize> order;
    for (usize i = 0; i < 8; ++i) {
        pool.submit([&order, i](usize worker) {
            REQUIRE(worker == 0);
            order.push_back(i);
        });
    }

    // Inline means finished by the time submit returns, and in submission
    // order. That is what makes the zero-worker pool the serial arm of a
    // parallel-versus-serial comparison rather than a different code path.
    REQUIRE(order.size() == 8);
    REQUIRE(std::is_sorted(order.begin(), order.end()));
    REQUIRE(pool.in_flight() == 0);
}

TEST_CASE("every job runs exactly once", "[job_pool]") {
    constexpr usize kJobs = 512;

    base::JobPool                        pool{4};
    std::array<std::atomic<u32>, kJobs>  ran{};

    for (usize i = 0; i < kJobs; ++i) {
        pool.submit([&ran, i](usize) { ran[i].fetch_add(1); });
    }
    pool.wait_idle();

    for (usize i = 0; i < kJobs; ++i) {
        REQUIRE(ran[i].load() == 1);
    }
    REQUIRE(pool.in_flight() == 0);
}

TEST_CASE("a worker index is never used by two jobs at once", "[job_pool]") {
    // The whole reason the index exists: a caller keeps per-worker state and
    // reaches it without a lock. If two jobs could ever hold the same index
    // simultaneously, every such caller would have a data race and no way to
    // see it.
    constexpr usize kWorkers = 4;

    base::JobPool                 pool{kWorkers};
    std::array<std::atomic<i32>, kWorkers> busy{};
    std::atomic<i32>              overlaps{0};
    std::atomic<u32>              seen_mask{0};

    for (usize i = 0; i < 400; ++i) {
        pool.submit([&](usize worker) {
            REQUIRE(worker < kWorkers);
            seen_mask.fetch_or(1U << worker);
            if (busy[worker].fetch_add(1) != 0) {
                overlaps.fetch_add(1);
            }
            // Long enough that an overlap would be caught, short enough that
            // the test stays a test.
            volatile i32 spin = 0;
            for (i32 k = 0; k < 2000; ++k) {
                spin += k;
            }
            busy[worker].fetch_sub(1);
        });
    }
    pool.wait_idle();

    REQUIRE(overlaps.load() == 0);
    // Which workers happened to be used is deliberately *not* asserted here.
    // Four hundred short jobs can legitimately be drained by one thread on a
    // loaded machine, and this test failed exactly that way once — reporting
    // "1 == 15" while the property it exists to check, that no index is ever
    // held twice at once, was never in doubt. The next test asks the same
    // question in a way the scheduler cannot answer wrongly.
    (void)seen_mask;
}

TEST_CASE("every worker index belongs to a thread that really runs", "[job_pool]") {
    // The claim the overlap test used to make as a side effect, made properly:
    // a pool that quietly ran everything on thread zero would pass the overlap
    // check and be useless.
    //
    // Each job holds its worker until every other one has arrived, so the only
    // way to see all four indices is for all four threads to be running at the
    // same time. No amount of load changes the answer — it only changes how
    // long it takes — and the deadline is there so a broken pool fails instead
    // of hanging the suite.
    constexpr usize kWorkers = 4;

    base::JobPool      pool{kWorkers};
    std::atomic<usize> arrived{0};
    std::atomic<u32>   seen{0};

    for (usize i = 0; i < kWorkers; ++i) {
        pool.submit([&](usize worker) {
            seen.fetch_or(1U << worker);
            arrived.fetch_add(1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (arrived.load() < kWorkers && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        });
    }
    pool.wait_idle();

    REQUIRE(seen.load() == (1U << kWorkers) - 1U);
}

TEST_CASE("destroying a pool with a backlog does not hang", "[job_pool]") {
    std::atomic<i32> ran{0};
    {
        base::JobPool pool{2};
        for (usize i = 0; i < 10000; ++i) {
            pool.submit([&ran](usize) { ran.fetch_add(1); });
        }
        // Leaves the scope with most of the queue unstarted. Queued jobs are
        // dropped on purpose — a shutdown that waits for terrain nobody will
        // see is a hang, not a courtesy.
    }
    REQUIRE(ran.load() <= 10000);
}
