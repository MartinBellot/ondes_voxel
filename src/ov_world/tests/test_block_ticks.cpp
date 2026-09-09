#include "ov/world/block_ticks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>

using namespace ov;
using namespace ov::world;

namespace {

[[nodiscard]] const std::optional<registry::BlockRegistry>& blocks() {
    static const std::optional<registry::BlockRegistry> loaded = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        auto result = registry::BlockRegistry::load(path);
        return result ? std::optional{std::move(*result)} : std::nullopt;
    }();
    return loaded;
}

[[nodiscard]] registry::BlockId block_of(std::string_view name) {
    const auto id = blocks()->find_block(name);
    REQUIRE(id.has_value());
    return *id;
}

}  // namespace

TEST_CASE("a tick fires on the tick it was asked for", "[world][ticks]") {
    BlockTickScheduler queue;
    const registry::BlockId repeater{7};

    REQUIRE(queue.schedule(BlockPos{1, 2, 3}, repeater, 100, 4));
    REQUIRE(queue.pending_count() == 1);
    REQUIRE(queue.is_scheduled(BlockPos{1, 2, 3}, repeater));

    std::vector<ScheduledTick> due;
    queue.drain_due(103, due);
    REQUIRE(due.empty());
    queue.drain_due(104, due);
    REQUIRE(due.size() == 1);
    REQUIRE(due[0].pos == BlockPos{1, 2, 3});
    REQUIRE(queue.pending_count() == 0);
}

TEST_CASE("one tick per position per block, first request wins", "[world][ticks]") {
    BlockTickScheduler queue;
    const registry::BlockId wire{1};
    const registry::BlockId water{2};

    REQUIRE(queue.schedule(BlockPos{0, 0, 0}, wire, 0, 5));
    // A second request for the same block at the same place changes nothing —
    // which is what stops a wire seeing six neighbour updates from queuing six
    // ticks, and stops a repeater's delay from being reset by a twitch.
    REQUIRE_FALSE(queue.schedule(BlockPos{0, 0, 0}, wire, 0, 1));
    // A different block at the same place is a different tick.
    REQUIRE(queue.schedule(BlockPos{0, 0, 0}, water, 0, 1));
    REQUIRE(queue.pending_count() == 2);

    std::vector<ScheduledTick> due;
    queue.drain_due(1, due);
    REQUIRE(due.size() == 1);
    REQUIRE(due[0].block == water);
}

TEST_CASE("priority orders ticks inside one game tick", "[world][ticks]") {
    BlockTickScheduler queue;
    const registry::BlockId repeater{7};

    // Same target tick, different priorities, queued in the wrong order.
    queue.schedule(BlockPos{0, 0, 0}, repeater, 0, 2, TickPriority::VeryLow);
    queue.schedule(BlockPos{1, 0, 0}, repeater, 0, 2, TickPriority::ExtremelyHigh);
    queue.schedule(BlockPos{2, 0, 0}, repeater, 0, 2, TickPriority::Normal);
    queue.schedule(BlockPos{3, 0, 0}, repeater, 0, 2, TickPriority::VeryHigh);

    std::vector<ScheduledTick> due;
    queue.drain_due(2, due);
    REQUIRE(due.size() == 4);
    REQUIRE(due[0].pos.x == 1);  // -3
    REQUIRE(due[1].pos.x == 3);  // -2
    REQUIRE(due[2].pos.x == 2);  //  0
    REQUIRE(due[3].pos.x == 0);  //  2
}

TEST_CASE("a tie is broken by the order the ticks were asked for", "[world][ticks]") {
    BlockTickScheduler queue;
    const registry::BlockId repeater{7};
    for (i32 x = 5; x >= 0; --x) {
        queue.schedule(BlockPos{x, 0, 0}, repeater, 0, 1);
    }
    std::vector<ScheduledTick> due;
    queue.drain_due(1, due);
    REQUIRE(due.size() == 6);
    for (usize i = 0; i < due.size(); ++i) {
        REQUIRE(due[i].pos.x == 5 - static_cast<i32>(i));
    }
}

TEST_CASE("cancelling drops the tick and nothing else", "[world][ticks]") {
    BlockTickScheduler queue;
    const registry::BlockId a{1};
    const registry::BlockId b{2};
    queue.schedule(BlockPos{0, 0, 0}, a, 0, 1);
    queue.schedule(BlockPos{0, 0, 0}, b, 0, 1);
    queue.schedule(BlockPos{1, 0, 0}, a, 0, 1);

    REQUIRE(queue.cancel(BlockPos{0, 0, 0}, a));
    REQUIRE_FALSE(queue.cancel(BlockPos{0, 0, 0}, a));
    REQUIRE(queue.pending_count() == 2);
    REQUIRE(queue.cancel_all_at(BlockPos{0, 0, 0}) == 1);
    REQUIRE(queue.pending_count() == 1);
    REQUIRE(queue.is_scheduled(BlockPos{1, 0, 0}, a));
}

TEST_CASE("the drain limit leaves the rest for the next tick", "[world][ticks]") {
    BlockTickScheduler queue;
    const registry::BlockId a{1};
    for (i32 x = 0; x < 10; ++x) {
        queue.schedule(BlockPos{x, 0, 0}, a, 0, 0);
    }
    std::vector<ScheduledTick> due;
    queue.drain_due(0, due, 4);
    REQUIRE(due.size() == 4);
    REQUIRE(queue.pending_count() == 6);
}

TEST_CASE("block_ticks survives a round trip through the save format",
          "[world][ticks][registry]") {
    if (!blocks().has_value()) {
        SKIP("registry.ovpack not built");
    }
    const registry::BlockId repeater = block_of("minecraft:repeater");
    const registry::BlockId wire     = block_of("minecraft:redstone_wire");

    BlockTickScheduler queue;
    queue.schedule(BlockPos{5, 64, -3}, repeater, 1000, 8, TickPriority::VeryHigh);
    queue.schedule(BlockPos{6, 64, -3}, wire, 1000, 0, TickPriority::ExtremelyHigh);
    // In another chunk, so it must not be written into this one's list.
    queue.schedule(BlockPos{40, 64, -3}, repeater, 1000, 2);

    const nbt::Tag list = queue.save_chunk(ChunkPos{0, -1}, 1000, *blocks());
    REQUIRE(list.list() != nullptr);
    REQUIRE(list.list()->size() == 2);

    // `t` is a delay against the level's own clock, not an absolute tick: a
    // world reloaded a month later must not fire a month of backlog at once.
    const nbt::Tag& first = list.list()->front();
    REQUIRE(first.find("i")->as_string() == "minecraft:redstone_wire");
    REQUIRE(first.find("t")->as_i64() == 0);
    REQUIRE(first.find("p")->as_i64() == -3);
    REQUIRE(first.find("x")->as_i64() == 6);
    REQUIRE(first.find("y")->as_i64() == 64);
    REQUIRE(first.find("z")->as_i64() == -3);

    BlockTickScheduler reloaded;
    usize              skipped = 0;
    REQUIRE(reloaded.load_chunk(list, 5000, *blocks(), skipped) == 2);
    REQUIRE(skipped == 0);

    std::vector<ScheduledTick> back;
    reloaded.snapshot(back);
    REQUIRE(back.size() == 2);
    REQUIRE(back[0].pos == BlockPos{6, 64, -3});
    REQUIRE(back[0].trigger_tick == 5000);
    REQUIRE(back[1].pos == BlockPos{5, 64, -3});
    REQUIRE(back[1].trigger_tick == 5008);
    REQUIRE(back[1].priority == TickPriority::VeryHigh);
}

TEST_CASE("a tick for a block this version does not have is refused and counted",
          "[world][ticks][registry]") {
    if (!blocks().has_value()) {
        SKIP("registry.ovpack not built");
    }
    nbt::Tag list  = nbt::Tag::make_list(nbt::TagType::Compound);
    nbt::Tag entry = nbt::Tag::make_compound();
    entry.compound()->push_back(nbt::CompoundEntry{"i", nbt::Tag{std::string{"modded:widget"}}});
    entry.compound()->push_back(nbt::CompoundEntry{"t", nbt::Tag{i32{3}}});
    entry.compound()->push_back(nbt::CompoundEntry{"p", nbt::Tag{i32{0}}});
    entry.compound()->push_back(nbt::CompoundEntry{"x", nbt::Tag{i32{0}}});
    entry.compound()->push_back(nbt::CompoundEntry{"y", nbt::Tag{i32{0}}});
    entry.compound()->push_back(nbt::CompoundEntry{"z", nbt::Tag{i32{0}}});
    list.list()->push_back(std::move(entry));

    BlockTickScheduler queue;
    usize              skipped = 0;
    REQUIRE(queue.load_chunk(list, 0, *blocks(), skipped) == 0);
    REQUIRE(skipped == 1);
}
