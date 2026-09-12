// ── streaming ── Which block the chunk source generates next, and which never.
//
// What these tests settle is the scheduling, not the terrain: a stand-in
// generator costs nothing, records the order it was asked in, and can hold its
// first block while the test changes its mind about what is wanted — exactly
// what a player flying on does to the server. The terrain itself is proved
// elsewhere: `ov_gendet` generates the same squares through this source and
// serially, and compares every cell (docs/provenance/chunkmap.md § 4).

#include "../src/async_chunk_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

/// A generator whose blocks cost nothing and which remembers who was first.
struct FakeGenerator {
    std::mutex              mutex;
    std::condition_variable gate;
    bool                    hold_first{true};
    bool                    released{false};
    std::vector<u64>        order;  ///< block coordinates, packed, in start order

    [[nodiscard]] AsyncChunkSource::Generate generate() {
        return [this](usize /*stack*/, i32 x, i32 z, i32 side,
                      std::vector<std::pair<ChunkPos, world::Chunk>>& /*out*/) {
            std::unique_lock lock{mutex};
            // The origin is a whole number of blocks: the division is exact.
            order.push_back(ChunkPos{x / side, z / side}.packed());
            if (hold_first) {
                hold_first = false;
                gate.wait(lock, [this] { return released; });
            }
        };
    }

    void release() {
        {
            const std::scoped_lock lock{mutex};
            released = true;
        }
        gate.notify_all();
    }

    [[nodiscard]] usize started() {
        const std::scoped_lock lock{mutex};
        return order.size();
    }
};

/// A chunk somewhere inside block (bx, bz).
[[nodiscard]] ChunkPos inside(i32 bx, i32 bz) {
    return ChunkPos{bx * AsyncChunkSource::kBlockChunks + 1, bz * AsyncChunkSource::kBlockChunks + 2};
}

[[nodiscard]] u64 block(i32 bx, i32 bz) { return ChunkPos{bx, bz}.packed(); }

template <typename Predicate>
[[nodiscard]] bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

}  // namespace

TEST_CASE("a free worker takes the best block of the latest list, not of the first",
          "[server][streaming]") {
    FakeGenerator    fake;
    AsyncChunkSource source{fake.generate(), 1};

    // One worker. The first block starts at once and is held there.
    const std::vector<ChunkPos> before{inside(0, 0), inside(5, 5), inside(6, 6)};
    source.prioritise(before);
    REQUIRE(eventually([&] { return fake.started() == 1; }));

    // The player flew on. Nearest first, from where they are now; (5, 5) is
    // behind them and no longer wanted.
    const std::vector<ChunkPos> after{inside(9, 9), inside(8, 8), inside(6, 6)};
    source.prioritise(after);
    fake.release();
    REQUIRE(eventually([&] { return source.blocks_done() == 4; }));

    std::vector<GeneratedBlock> out;
    CHECK(source.drain(out) == 4);
    const std::scoped_lock lock{fake.mutex};
    REQUIRE(fake.order.size() == 4);
    CHECK(fake.order[0] == block(0, 0));  // already running: finished, not dropped
    CHECK(fake.order[1] == block(9, 9));
    CHECK(fake.order[2] == block(8, 8));
    CHECK(fake.order[3] == block(6, 6));
    CHECK(std::ranges::find(fake.order, block(5, 5)) == fake.order.end());  // cancelled
}

TEST_CASE("a block running or finished is never started twice", "[server][streaming]") {
    FakeGenerator    fake;
    AsyncChunkSource source{fake.generate(), 2};

    // Several chunks of the same two blocks, asked for again and again while
    // the first is held: the list collapses to blocks, and a block already
    // running or waiting to be drained is not queued again.
    const std::vector<ChunkPos> wanted{inside(1, 1), ChunkPos{4, 4}, inside(2, 1), ChunkPos{5, 6}};
    for (int tick = 0; tick < 10; ++tick) {
        source.prioritise(wanted);
    }
    fake.release();
    REQUIRE(eventually([&] { return source.blocks_done() == 2; }));
    for (int tick = 0; tick < 10; ++tick) {
        source.prioritise(wanted);  // finished, not drained: still skipped
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(source.blocks_done() == 2);
    CHECK(source.in_flight() == 0);

    std::vector<GeneratedBlock> out;
    CHECK(source.drain(out) == 2);
    const std::scoped_lock lock{fake.mutex};
    CHECK(fake.order.size() == 2);
}

TEST_CASE("request refuses what it already has, and a full list", "[server][streaming]") {
    FakeGenerator    fake;
    AsyncChunkSource source{fake.generate(), 1};

    CHECK(source.request(inside(0, 0)));
    REQUIRE(eventually([&] { return fake.started() == 1; }));
    CHECK_FALSE(source.request(inside(0, 0)));  // running
    CHECK(source.request(inside(1, 0)));
    CHECK_FALSE(source.request(ChunkPos{5, 2}));  // waiting: same block as inside(1, 0)
    CHECK(source.request(inside(2, 0)));
    CHECK(source.request(inside(3, 0)));
    CHECK_FALSE(source.request(inside(4, 0)));  // three waiting per worker, and one worker
    fake.release();
    REQUIRE(eventually([&] { return source.blocks_done() == 4; }));
    std::vector<GeneratedBlock> out;
    CHECK(source.drain(out) == 4);
}
