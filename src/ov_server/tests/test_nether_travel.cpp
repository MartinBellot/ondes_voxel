// The server's side of the Nether: the portal clock, the two levels' names,
// and the portal index read back out of a saved chunk.
#include "../src/nether_travel.hpp"

#include "ov/nbt/tag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::server;

TEST_CASE("a survival player crosses after 80 ticks, a creative one at once", "[nether]") {
    PortalTimer survival;
    i32         ticks = 0;
    while (!survival.tick(true, false)) {
        ++ticks;
        REQUIRE(ticks < 1000);
    }
    CHECK(ticks == gameplay::kPortalWaitSurvival);
    CHECK(survival.cooldown == gameplay::kPortalCooldown);

    PortalTimer creative;
    ticks = 0;
    while (!creative.tick(true, true)) {
        ++ticks;
    }
    CHECK(ticks == gameplay::kPortalWaitCreative);
}

TEST_CASE("the cooldown holds while the player stays in the portal", "[nether]") {
    PortalTimer timer;
    timer.cooldown = gameplay::kPortalCooldown;
    for (i32 i = 0; i < 1000; ++i) {
        REQUIRE_FALSE(timer.tick(true, true));
    }
    CHECK(timer.cooldown == gameplay::kPortalCooldown);
    // Out of it, the cooldown runs down; then the portal takes them again.
    for (i32 i = 0; i < gameplay::kPortalCooldown; ++i) {
        (void)timer.tick(false, true);
    }
    CHECK(timer.cooldown == 0);
    (void)timer.tick(true, true);
    CHECK(timer.tick(true, true));
}

TEST_CASE("stepping out walks the clock back four ticks at a time", "[nether]") {
    PortalTimer timer;
    for (i32 i = 0; i < 40; ++i) {
        REQUIRE_FALSE(timer.tick(true, false));
    }
    CHECK(timer.time == 40);
    (void)timer.tick(false, false);
    CHECK(timer.time == 36);
    for (i32 i = 0; i < 20; ++i) {
        (void)timer.tick(false, false);
    }
    CHECK(timer.time == 0);
}

TEST_CASE("the server has three levels, and refuses any other by name", "[nether]") {
    CHECK(dimension_by_name("minecraft:overworld") == DimensionId::Overworld);
    CHECK(dimension_by_name("minecraft:the_nether") == DimensionId::Nether);
    // ── end ──
    CHECK(dimension_by_name("minecraft:the_end") == DimensionId::End);
    CHECK_FALSE(dimension_by_name("mydatapack:moon").has_value());
    const DimensionInfo& end = dimension_info(DimensionId::End);
    CHECK(end.type == "minecraft:the_end");
    CHECK(end.shape.min_y == 0);
    CHECK(end.shape.height == 256);
    CHECK_FALSE(end.traits.ultrawarm);
    CHECK_FALSE(end.traits.natural);
    CHECK(end.region_dir == "DIM1/region");
    const DimensionInfo& nether = dimension_info(DimensionId::Nether);
    CHECK(nether.shape.min_y == 0);
    CHECK(nether.shape.height == 256);
    CHECK(nether.traits.ultrawarm);
    CHECK(nether.coordinate_scale == 8.0);
    CHECK(nether.portal_top == 127);
    CHECK(nether.search_radius == 16);
    CHECK(nether.region_dir == "DIM-1/region");
    CHECK(dimension_info(DimensionId::Overworld).search_radius == 128);
}

TEST_CASE("portal blocks are found in a saved chunk's palette", "[nether]") {
    // A section with a two-entry palette: netherrack everywhere but one cell.
    const auto entry = [](std::string name) {
        nbt::Tag tag = nbt::Tag::make_compound();
        (void)tag.put("Name", nbt::Tag{std::move(name)});
        return tag;
    };
    nbt::Tag palette = nbt::Tag::make_list(nbt::TagType::Compound);
    (void)palette.push(entry("minecraft:netherrack"));
    (void)palette.push(entry("minecraft:nether_portal"));
    // Four bits a cell, sixteen cells a long. Cell 273 = x 1, z 1, y 1.
    nbt::Tag::LongArray data(256, 0);
    data[273 / 16] = static_cast<i64>(1ULL << ((273 % 16) * 4));
    nbt::Tag states = nbt::Tag::make_compound();
    (void)states.put("palette", palette);
    (void)states.put("data", nbt::Tag{data});
    nbt::Tag section = nbt::Tag::make_compound();
    (void)section.put("Y", nbt::Tag{static_cast<i8>(4)});
    (void)section.put("block_states", states);
    nbt::Tag sections = nbt::Tag::make_list(nbt::TagType::Compound);
    (void)sections.push(section);
    nbt::Document chunk;
    chunk.root = nbt::Tag::make_compound();
    (void)chunk.root.put("xPos", nbt::Tag{static_cast<i32>(-2)});
    (void)chunk.root.put("zPos", nbt::Tag{static_cast<i32>(3)});
    (void)chunk.root.put("sections", sections);

    std::vector<BlockPos> found;
    portal_blocks_in(chunk, found);
    REQUIRE(found.size() == 1);
    CHECK(found[0] == BlockPos{-2 * 16 + 1, 4 * 16 + 1, 3 * 16 + 1});
}
