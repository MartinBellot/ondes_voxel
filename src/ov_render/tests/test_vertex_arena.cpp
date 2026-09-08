#include "ov/render/vertex_arena.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace {

/// Two ranges must never overlap. This is the property the whole thing exists
/// for, and the one whose failure looks like corrupted geometry rather than a
/// crash — so it is checked directly rather than inferred.
[[nodiscard]] bool disjoint(const std::vector<std::pair<u64, u64>>& live) {
    auto sorted = live;
    std::ranges::sort(sorted);
    for (usize i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1].first + sorted[i - 1].second > sorted[i].first) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("the page size divides the terrain vertex", "[render][arena]") {
    // Not a style preference: an indirect command's vertexOffset counts
    // vertices, so an allocation that does not start on a whole vertex draws
    // the terrain shifted by a fraction of one.
    REQUIRE(kArenaPageBytes % 12 == 0);
    REQUIRE(kArenaPageBytes / 12 == 256);
}

TEST_CASE("an empty arena hands out nothing", "[render][arena]") {
    VertexArena arena(0);
    REQUIRE(arena.allocate(16) == VertexArena::kNoSpace);
    REQUIRE(arena.capacity() == 0);
}

TEST_CASE("allocations are page aligned and never overlap", "[render][arena]") {
    VertexArena arena(kArenaPageBytes * 8);

    const u64 a = arena.allocate(10);      // one page
    const u64 b = arena.allocate(3073);    // two pages
    const u64 c = arena.allocate(kArenaPageBytes);

    REQUIRE(a != VertexArena::kNoSpace);
    REQUIRE(b != VertexArena::kNoSpace);
    REQUIRE(c != VertexArena::kNoSpace);
    REQUIRE(a % kArenaPageBytes == 0);
    REQUIRE(b % kArenaPageBytes == 0);
    REQUIRE(c % kArenaPageBytes == 0);
    REQUIRE(disjoint({{a, kArenaPageBytes}, {b, kArenaPageBytes * 2}, {c, kArenaPageBytes}}));
    REQUIRE(arena.used() == kArenaPageBytes * 4);
}

TEST_CASE("a full arena refuses rather than overlapping", "[render][arena]") {
    VertexArena arena(kArenaPageBytes * 2);
    REQUIRE(arena.allocate(kArenaPageBytes * 2) == 0);
    REQUIRE(arena.allocate(1) == VertexArena::kNoSpace);
    REQUIRE(arena.used() == arena.capacity());
}

TEST_CASE("neighbouring frees coalesce back into one block", "[render][arena]") {
    VertexArena arena(kArenaPageBytes * 4);
    const u64   a = arena.allocate(kArenaPageBytes);
    const u64   b = arena.allocate(kArenaPageBytes);
    const u64   c = arena.allocate(kArenaPageBytes);
    const u64   d = arena.allocate(kArenaPageBytes);
    REQUIRE(arena.free_block_count() == 0);

    // Free the outer two first, so that freeing the middle has to merge on
    // both sides at once — the case a naive implementation gets wrong.
    arena.release(a, kArenaPageBytes);
    arena.release(d, kArenaPageBytes);
    REQUIRE(arena.free_block_count() == 2);

    arena.release(c, kArenaPageBytes);
    arena.release(b, kArenaPageBytes);
    REQUIRE(arena.free_block_count() == 1);
    REQUIRE(arena.largest_free() == arena.capacity());
    REQUIRE(arena.used() == 0);

    // And it is genuinely one block again, not merely counted as one.
    REQUIRE(arena.allocate(kArenaPageBytes * 4) == 0);
}

TEST_CASE("a released range can be handed out again", "[render][arena]") {
    VertexArena arena(kArenaPageBytes * 3);
    const u64   a = arena.allocate(kArenaPageBytes);
    (void)arena.allocate(kArenaPageBytes);
    arena.release(a, kArenaPageBytes);
    REQUIRE(arena.allocate(kArenaPageBytes) == a);
}

TEST_CASE("churn never hands the same byte to two owners", "[render][arena]") {
    // The pattern a chunk stream actually produces: allocate a lot, free a
    // scattered half, allocate again into the holes, repeat. A fixed seed, so
    // a failure is reproducible.
    VertexArena  arena(kArenaPageBytes * 512);
    std::mt19937 random(20200101);

    std::vector<std::pair<u64, u64>> live;
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < 30; ++i) {
            const u64 size   = 1 + random() % (kArenaPageBytes * 4);
            const u64 offset = arena.allocate(size);
            if (offset != VertexArena::kNoSpace) {
                live.emplace_back(offset, arena.round_up(size));
            }
        }
        REQUIRE(disjoint(live));

        for (usize i = live.size(); i-- > 0;) {
            if (random() % 2 == 0) {
                arena.release(live[i].first, live[i].second);
                live.erase(live.begin() + static_cast<isize>(i));
            }
        }
    }

    u64 held = 0;
    for (const auto& [offset, size] : live) {
        held += size;
    }
    REQUIRE(arena.used() == held);

    for (const auto& [offset, size] : live) {
        arena.release(offset, size);
    }
    REQUIRE(arena.used() == 0);
    REQUIRE(arena.free_block_count() == 1);
    REQUIRE(arena.largest_free() == arena.capacity());
}
