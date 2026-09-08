#include "ov/world/block_ticks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ov;
using namespace ov::world;

namespace {

[[nodiscard]] std::vector<std::string> names_of(const std::vector<ScheduledTick>& ticks) {
    std::vector<std::string> out;
    out.reserve(ticks.size());
    for (const ScheduledTick& tick : ticks) {
        out.push_back(tick.what);
    }
    return out;
}

}  // namespace

TEST_CASE("a scheduled tick comes due on the tick it was asked for", "[block_ticks]") {
    BlockTickScheduler         scheduler;
    std::vector<ScheduledTick> due;

    scheduler.schedule(BlockPos{0, 0, 0}, "minecraft:water", 5, 100);

    scheduler.collect_due(104, due);
    REQUIRE(due.empty());
    REQUIRE(scheduler.pending() == 1);

    scheduler.collect_due(105, due);
    REQUIRE(due.size() == 1);
    REQUIRE(due[0].pos == BlockPos{0, 0, 0});
    REQUIRE(due[0].what == "minecraft:water");
    REQUIRE(scheduler.pending() == 0);
}

TEST_CASE("the same position and name is only queued once", "[block_ticks]") {
    // Vanilla's hasScheduledTick check. Without it, a block whose six
    // neighbours all change on one tick schedules itself six times and then
    // spreads six blocks in one step.
    BlockTickScheduler scheduler;
    scheduler.schedule(BlockPos{1, 2, 3}, "minecraft:flowing_water", 5, 0);
    scheduler.schedule(BlockPos{1, 2, 3}, "minecraft:flowing_water", 1, 0);
    REQUIRE(scheduler.pending() == 1);

    // A different name at the same place is a different tick, and a fluid tick
    // and a block tick genuinely do coexist at one position.
    scheduler.schedule(BlockPos{1, 2, 3}, "minecraft:oak_leaves", 5, 0);
    REQUIRE(scheduler.pending() == 2);

    REQUIRE(scheduler.is_scheduled(BlockPos{1, 2, 3}, "minecraft:flowing_water"));
    REQUIRE_FALSE(scheduler.is_scheduled(BlockPos{1, 2, 4}, "minecraft:flowing_water"));
}

TEST_CASE("ties break on priority, then on insertion order", "[block_ticks]") {
    BlockTickScheduler         scheduler;
    std::vector<ScheduledTick> due;

    // All due on the same tick, deliberately inserted out of priority order.
    scheduler.schedule(BlockPos{0, 0, 0}, "normal_first", 1, 0, TickPriority::Normal);
    scheduler.schedule(BlockPos{1, 0, 0}, "very_low", 1, 0, TickPriority::VeryLow);
    scheduler.schedule(BlockPos{2, 0, 0}, "extremely_high", 1, 0, TickPriority::ExtremelyHigh);
    scheduler.schedule(BlockPos{3, 0, 0}, "normal_second", 1, 0, TickPriority::Normal);
    scheduler.schedule(BlockPos{4, 0, 0}, "high", 1, 0, TickPriority::High);

    scheduler.collect_due(1, due);
    // Lower priority value runs first, and the two Normals keep the order they
    // were scheduled in.
    REQUIRE(names_of(due) == std::vector<std::string>{"extremely_high", "high", "normal_first",
                                                      "normal_second", "very_low"});
}

TEST_CASE("an earlier tick outranks a higher priority", "[block_ticks]") {
    BlockTickScheduler         scheduler;
    std::vector<ScheduledTick> due;

    scheduler.schedule(BlockPos{0, 0, 0}, "late_but_urgent", 5, 0, TickPriority::ExtremelyHigh);
    scheduler.schedule(BlockPos{1, 0, 0}, "early", 1, 0, TickPriority::ExtremelyLow);

    scheduler.collect_due(10, due);
    REQUIRE(names_of(due) == std::vector<std::string>{"early", "late_but_urgent"});
}

TEST_CASE("collect_due leaves everything not yet due", "[block_ticks]") {
    BlockTickScheduler         scheduler;
    std::vector<ScheduledTick> due;

    for (i32 i = 0; i < 10; ++i) {
        scheduler.schedule(BlockPos{i, 0, 0}, "minecraft:water", i, 0);
    }
    scheduler.collect_due(4, due);
    REQUIRE(due.size() == 5);
    REQUIRE(scheduler.pending() == 5);
    scheduler.collect_due(100, due);
    REQUIRE(due.size() == 5);
    REQUIRE(scheduler.pending() == 0);
}

TEST_CASE("unloading a column forgets only its own ticks", "[block_ticks]") {
    BlockTickScheduler scheduler;
    scheduler.schedule(BlockPos{0, 0, 0}, "a", 1, 0);      // chunk 0,0
    scheduler.schedule(BlockPos{15, 0, 15}, "b", 1, 0);    // chunk 0,0
    scheduler.schedule(BlockPos{16, 0, 0}, "c", 1, 0);     // chunk 1,0
    scheduler.schedule(BlockPos{-1, 0, 0}, "d", 1, 0);     // chunk -1,0 — floor division
    REQUIRE(scheduler.pending() == 4);

    scheduler.forget_chunk(0, 0);
    REQUIRE(scheduler.pending() == 2);
    REQUIRE(scheduler.is_scheduled(BlockPos{16, 0, 0}, "c"));
    REQUIRE(scheduler.is_scheduled(BlockPos{-1, 0, 0}, "d"));
}

TEST_CASE("the Anvil round trip keeps position, name, priority and delay", "[block_ticks]") {
    // The shape is measured, not assumed: a save from a real 1.20.1 server
    // carries {i, p, t, x, y, z} per entry, with `t` a delay relative to the
    // chunk's game time. See docs/provenance/fluides.md.
    BlockTickScheduler scheduler;
    scheduler.schedule(BlockPos{3, -60, 7}, "minecraft:flowing_water", 5, 1000);
    scheduler.schedule(BlockPos{4, -60, 7}, "minecraft:flowing_lava", 30, 1000,
                       TickPriority::VeryHigh);

    const nbt::Tag list = ticks_to_nbt(scheduler.snapshot(), 0, 0, 1000);
    REQUIRE(list.type() == nbt::TagType::List);
    REQUIRE(list.size() == 2);

    const nbt::Tag& first = (*list.list())[0];
    REQUIRE(first.find("i")->as_string() == "minecraft:flowing_water");
    REQUIRE(first.find("t")->as_i64() == 5);
    REQUIRE(first.find("p")->as_i64() == 0);
    REQUIRE(first.find("x")->as_i64() == 3);
    REQUIRE(first.find("y")->as_i64() == -60);
    REQUIRE(first.find("z")->as_i64() == 7);
    REQUIRE((*list.list())[1].find("p")->as_i64() == -2);

    std::vector<ScheduledTick> read;
    REQUIRE(ticks_from_nbt(list, 2000, read));
    REQUIRE(read.size() == 2);
    // The delay was relative, so re-reading at a different game time moves the
    // absolute tick with it. That is the point of storing a delay.
    REQUIRE(read[0].when == 2005);
    REQUIRE(read[1].when == 2030);
    REQUIRE(read[1].priority == TickPriority::VeryHigh);
}

TEST_CASE("serialising a chunk column takes only that column's ticks", "[block_ticks]") {
    BlockTickScheduler scheduler;
    scheduler.schedule(BlockPos{0, 0, 0}, "a", 1, 0);
    scheduler.schedule(BlockPos{16, 0, 0}, "b", 1, 0);

    REQUIRE(ticks_to_nbt(scheduler.snapshot(), 0, 0, 0).size() == 1);
    REQUIRE(ticks_to_nbt(scheduler.snapshot(), 1, 0, 0).size() == 1);
    REQUIRE(ticks_to_nbt(scheduler.snapshot(), 5, 5, 0).size() == 0);
}

TEST_CASE("a malformed tick list is refused rather than guessed", "[block_ticks]") {
    std::vector<ScheduledTick> out;

    // Not a list at all.
    REQUIRE_FALSE(ticks_from_nbt(nbt::Tag{i32{7}}, 0, out));

    // A compound missing its position: a tick whose place had to be guessed is
    // a block that updates somewhere else, with nothing to say where the
    // mistake came from.
    nbt::Tag list  = nbt::Tag::make_list(nbt::TagType::Compound);
    nbt::Tag entry = nbt::Tag::make_compound();
    entry.put("i", nbt::Tag{std::string{"minecraft:water"}});
    entry.put("t", nbt::Tag{i32{5}});
    entry.put("x", nbt::Tag{i32{0}});
    entry.put("y", nbt::Tag{i32{0}});
    list.push(std::move(entry));
    REQUIRE_FALSE(ticks_from_nbt(list, 0, out));

    // A priority outside the -3..3 the format defines.
    nbt::Tag bad_priority = nbt::Tag::make_list(nbt::TagType::Compound);
    nbt::Tag with_bad     = nbt::Tag::make_compound();
    with_bad.put("i", nbt::Tag{std::string{"minecraft:water"}});
    with_bad.put("p", nbt::Tag{i32{9}});
    with_bad.put("t", nbt::Tag{i32{5}});
    with_bad.put("x", nbt::Tag{i32{0}});
    with_bad.put("y", nbt::Tag{i32{0}});
    with_bad.put("z", nbt::Tag{i32{0}});
    bad_priority.push(std::move(with_bad));
    REQUIRE_FALSE(ticks_from_nbt(bad_priority, 0, out));
}

TEST_CASE("an empty list round-trips", "[block_ticks]") {
    // Vanilla writes an empty list with element type End, so the element type
    // may only be checked when there is something in it.
    std::vector<ScheduledTick> out;
    REQUIRE(ticks_from_nbt(nbt::Tag::make_list(nbt::TagType::End), 0, out));
    REQUIRE(out.empty());
}

TEST_CASE("adopted ticks keep their list order and sort after existing ones", "[block_ticks]") {
    BlockTickScheduler scheduler;
    std::vector<ScheduledTick> loaded{
        ScheduledTick{BlockPos{0, 0, 0}, "first", 10, TickPriority::Normal, 0},
        ScheduledTick{BlockPos{1, 0, 0}, "second", 10, TickPriority::Normal, 0},
        ScheduledTick{BlockPos{2, 0, 0}, "third", 10, TickPriority::Normal, 0},
    };
    scheduler.adopt(std::move(loaded));

    std::vector<ScheduledTick> due;
    scheduler.collect_due(10, due);
    REQUIRE(names_of(due) == std::vector<std::string>{"first", "second", "third"});
}
