#include "ov/base/thread.hpp"

#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace ov;

TEST_CASE("set_thread_role names the calling thread", "[thread]") {
    set_thread_role("ov-test", ThreadRole::Worker);
    REQUIRE(current_thread_name() == "ov-test");
}

TEST_CASE("thread names survive the platform length limits", "[thread]") {
    // Linux truncates at 15 characters; the name we report back is our own copy
    // and stays intact, which is what the log lines use.
    std::thread t{[] {
        set_thread_role("a-very-long-thread-name-indeed", ThreadRole::Background);
        REQUIRE(current_thread_name() == "a-very-long-thread-name-indeed");
    }};
    t.join();
}

TEST_CASE("an unnamed thread reports a placeholder rather than an empty view", "[thread]") {
    std::thread t{[] { REQUIRE(current_thread_name() == "?"); }};
    t.join();
}

TEST_CASE("worker count is capped at four", "[thread]") {
    const unsigned workers = recommended_worker_count();

    REQUIRE(workers >= 1);
    // Not a core count: on unified-memory hardware, memory bandwidth saturates
    // before the cores do, and meshing throughput falls past four workers.
    // Raising this needs a benchmark on the target machine.
    REQUIRE(workers <= 4);
}
