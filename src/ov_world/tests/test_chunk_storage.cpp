#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::world;

TEST_CASE("a chunk declares the version that wrote it", "[world][storage][version]") {
    // Read before anything else, and read on its own: a file from another
    // version may use shapes this one does not have, and vanilla upgrades old
    // saves through a converter this project does not implement. The only
    // honest answers are "this version" and "refuse" — and refusing has to
    // happen before anything is written back, because replacing a chunk we
    // failed to understand is how a save gets destroyed by the program meant
    // to open it.
    nbt::Document document;
    document.root = nbt::Tag::make_compound();
    REQUIRE_FALSE(chunk_data_version(document).has_value());

    document.root.compound()->push_back(
        nbt::CompoundEntry{"DataVersion", nbt::Tag{kDataVersion1201}});
    REQUIRE(chunk_data_version(document) == kDataVersion1201);

    // A future version reads back as itself rather than as an error, so the
    // caller can say which one it refused.
    document.root.compound()->clear();
    document.root.compound()->push_back(nbt::CompoundEntry{"DataVersion", nbt::Tag{i32{4082}}});
    REQUIRE(chunk_data_version(document) == 4082);
}


TEST_CASE("a chunk carries out the ticks that fall inside it", "[world][storage][ticks]") {
    // The two lists were written empty and unconditionally, so every pending
    // tick in a saved world was silently dropped: a water flow half-way across
    // a room resumed as a puddle that had stopped, and a repeater mid-delay
    // came back never having fired.
    BlockTickScheduler queue;
    queue.schedule(BlockPos{4, 60, 4}, "minecraft:water", 5, 100);
    queue.schedule(BlockPos{20, 60, 4}, "minecraft:water", 5, 100);  // the chunk next door
    queue.schedule(BlockPos{6, 61, 7}, "minecraft:repeater", 4, 100,
                   TickPriority::High);

    const std::vector<ScheduledTick> all = queue.snapshot();

    constexpr AirStates air{registry::BlockStateId{0}, registry::BlockStateId{12817}, registry::BlockStateId{12818}};
    Chunk chunk{ChunkPos{0, 0}, WorldShape::overworld(), air, nullptr};

    ChunkCodecContext context;
    context.block_ticks = all;
    context.game_time   = 100;

    nbt::Document document = to_nbt(chunk, context);
    const nbt::Tag* block_ticks = document.root.find("block_ticks");
    REQUIRE(block_ticks != nullptr);
    REQUIRE(block_ticks->list() != nullptr);

    // Two of the three, because `ticks_to_nbt` filters by column: handing the
    // whole level's queue to every chunk would write every pending tick in the
    // world into every region file.
    REQUIRE(block_ticks->list()->size() == 2);

    // `fluid_ticks` is still written, and still empty: a caller with no
    // scheduler must produce the shape it always did.
    const nbt::Tag* fluid_ticks = document.root.find("fluid_ticks");
    REQUIRE(fluid_ticks != nullptr);
    REQUIRE(fluid_ticks->list() != nullptr);
    REQUIRE(fluid_ticks->list()->empty());

    // And they come back, with the delay carried across a different "now" —
    // which is the whole point of storing `t` as a delay rather than an
    // absolute tick.
    BlockTickScheduler reloaded;
    REQUIRE(ticks_from_nbt(*block_ticks, 500, reloaded));
    REQUIRE(reloaded.pending() == 2);
    REQUIRE(reloaded.is_scheduled(BlockPos{4, 60, 4}, "minecraft:water"));
    REQUIRE(reloaded.is_scheduled(BlockPos{6, 61, 7}, "minecraft:repeater"));

    std::vector<ScheduledTick> due;
    reloaded.collect_due(503, due);
    // The repeater was four ticks out at save time, so it is due at 504 and not
    // at 503; the water was five out and is due at 505.
    REQUIRE(due.empty());
    reloaded.collect_due(505, due);
    REQUIRE(due.size() == 2);
    // Priority decides the order inside one drain, and High runs before Normal.
    REQUIRE(due[0].what == "minecraft:repeater");
}
