// The lock that is not a lock: it proves the world has one thread.
#include "../src/tick_thread_lock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <thread>

using namespace ov;
using namespace ov::server;

TEST_CASE("the owning thread locks, re-enters and try-locks without waiting", "[tick_lock]") {
    TickThreadLock a{TickThreadLock::Policy::Count};
    TickThreadLock b{TickThreadLock::Policy::Count};
    CHECK(a.owned_by_caller());
    {
        const std::scoped_lock both{a, b};  // std::lock over two of them
        const std::scoped_lock again{a};    // a std::mutex would deadlock here
        std::unique_lock       tried{b, std::try_to_lock};
        CHECK(tried.owns_lock());
    }
    CHECK(a.foreign_locks() == 0);
    CHECK(b.foreign_locks() == 0);
}

TEST_CASE("a lock taken from another thread is counted, not waited for", "[tick_lock]") {
    TickThreadLock world{TickThreadLock::Policy::Count};
    std::jthread   stranger{[&] {
        CHECK_FALSE(world.owned_by_caller());
        const std::scoped_lock touch{world};
        std::unique_lock       tried{world, std::try_to_lock};
    }};
    stranger.join();
    CHECK(world.foreign_locks() == 2);
    // The owner is still clean.
    const std::scoped_lock touch{world};
    CHECK(world.foreign_locks() == 2);
}

TEST_CASE("ownership moves to the thread that binds it", "[tick_lock]") {
    TickThreadLock world{TickThreadLock::Policy::Count};
    std::jthread   tick{[&] {
        world.bind_to_current_thread();
        const std::scoped_lock touch{world};
        CHECK(world.owned_by_caller());
    }};
    tick.join();
    CHECK(world.foreign_locks() == 0);
    // The constructing thread is now the stranger.
    const std::scoped_lock touch{world};
    CHECK(world.foreign_locks() == 1);
}
