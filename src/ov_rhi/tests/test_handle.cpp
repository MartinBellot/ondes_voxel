#include "ov/rhi/handle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::rhi;

namespace {

struct TestTag {};

using Pool       = HandlePool<std::string, TestTag>;
using TestHandle = Pool::HandleType;

}  // namespace

TEST_CASE("a default handle is invalid", "[rhi][handle]") {
    STATIC_REQUIRE(!TestHandle{}.valid());
    STATIC_REQUIRE(sizeof(TestHandle) == 8);

    Pool pool;
    CHECK(pool.get(TestHandle{}) == nullptr);
}

TEST_CASE("a handle finds its value", "[rhi][handle]") {
    Pool       pool;
    const auto a = pool.insert("first");
    const auto b = pool.insert("second");

    REQUIRE(pool.get(a) != nullptr);
    CHECK(*pool.get(a) == "first");
    CHECK(*pool.get(b) == "second");
    CHECK(pool.live_count() == 2);
}

TEST_CASE("a removed handle stops resolving", "[rhi][handle]") {
    Pool        pool;
    const auto  handle = pool.insert("gone");
    std::string removed;

    CHECK(pool.remove(handle, removed));
    CHECK(removed == "gone");
    CHECK(pool.get(handle) == nullptr);
    CHECK_FALSE(pool.remove(handle, removed));
    CHECK(pool.live_count() == 0);
}

TEST_CASE("a recycled slot does not resurrect the old handle", "[rhi][handle]") {
    // The whole reason handles are not pointers. A swapchain is destroyed and
    // recreated on every resize; something is always still holding the old
    // handle, and a raw pointer would make that a use-after-free that
    // reproduces once a week and never under a debugger.
    Pool        pool;
    const auto  first = pool.insert("first");
    std::string removed;
    REQUIRE(pool.remove(first, removed));

    const auto second = pool.insert("second");

    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    CHECK(pool.get(first) == nullptr);
    REQUIRE(pool.get(second) != nullptr);
    CHECK(*pool.get(second) == "second");
    // The slot was reused rather than appended to.
    CHECK(pool.capacity() == 1);
}

TEST_CASE("slots are reused rather than the array growing", "[rhi][handle]") {
    Pool        pool;
    std::string removed;
    for (int i = 0; i < 100; ++i) {
        const auto handle = pool.insert("transient");
        REQUIRE(pool.remove(handle, removed));
    }
    CHECK(pool.capacity() == 1);
    CHECK(pool.live_count() == 0);
}

TEST_CASE("for_each visits the live values only", "[rhi][handle]") {
    Pool       pool;
    const auto keep = pool.insert("keep");
    const auto drop = pool.insert("drop");

    std::string removed;
    REQUIRE(pool.remove(drop, removed));

    int         visited = 0;
    std::string seen;
    pool.for_each([&](std::string& value) {
        ++visited;
        seen = value;
    });

    CHECK(visited == 1);
    CHECK(seen == "keep");
    CHECK(pool.get(keep) != nullptr);
}
