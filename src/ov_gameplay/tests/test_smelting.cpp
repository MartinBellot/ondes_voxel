#include "ov/gameplay/smelting.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::Registries> registries;
    std::optional<RecipeBook>           book;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        auto   regs = registry::Registries::load(path);
        if (regs) {
            out.registries = std::move(*regs);
            out.book.emplace(*out.registries);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] registry::ProtocolId item_of(std::string_view name) {
    const auto items = loaded().registries->find("minecraft:item");
    REQUIRE(items.has_value());
    const auto id = loaded().registries->protocol_id(*items, name);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] std::string item_name(registry::ProtocolId id) {
    const auto items = loaded().registries->find("minecraft:item");
    REQUIRE(items.has_value());
    return std::string{loaded().registries->entry_of(*items, id)};
}

}  // namespace

TEST_CASE("burn times are the ones the game was seen to use", "[smelting]") {
    REQUIRE(loaded().book.has_value());
    const RecipeBook& book = *loaded().book;

    // Every number here came off a running 1.20.1 server: `BurnTime` read at a
    // dated tick, referred back to the ignition tick, which a per-tick command
    // block pinned. See docs/provenance/crafting-and-smelting.md.
    struct Case {
        const char* item;
        i32         furnace;
        i32         blast;
    };
    for (const Case& one : {Case{"minecraft:coal", 1600, 800},
                            Case{"minecraft:charcoal", 1600, 800},
                            Case{"minecraft:lava_bucket", 20000, 10000},
                            Case{"minecraft:blaze_rod", 2400, 1200},
                            Case{"minecraft:coal_block", 16000, 8000},
                            Case{"minecraft:oak_planks", 300, 150},
                            Case{"minecraft:stick", 100, 50},
                            Case{"minecraft:bamboo", 50, 25}}) {
        CHECK(burn_ticks(book, FurnaceKind::Furnace, item_of(one.item)) == one.furnace);
        CHECK(burn_ticks(book, FurnaceKind::BlastFurnace, item_of(one.item)) == one.blast);
    }

    // A dried kelp block is the item that proves the three tables are not one
    // table and a divisor: 4001 in a furnace, and 2000 — not 2000.5, not 2001 —
    // in a blast furnace.
    CHECK(burn_ticks(book, FurnaceKind::Furnace, item_of("minecraft:dried_kelp_block")) == 4001);
    CHECK(burn_ticks(book, FurnaceKind::BlastFurnace, item_of("minecraft:dried_kelp_block")) ==
          2000);

    // Not a fuel, and that is a measurement: every item in the registry was put
    // in a furnace and watched.
    CHECK(burn_ticks(book, FurnaceKind::Furnace, item_of("minecraft:stone")) == 0);
    CHECK(burn_ticks(book, FurnaceKind::Furnace, item_of("minecraft:iron_ingot")) == 0);
}

TEST_CASE("each furnace consults its own recipes", "[smelting]") {
    const RecipeBook& book = *loaded().book;
    const auto        ore  = item_of("minecraft:iron_ore");
    const auto        beef = item_of("minecraft:beef");
    const auto        cobble = item_of("minecraft:cobblestone");

    REQUIRE(match_cooking(book, FurnaceKind::Furnace, ore).has_value());
    REQUIRE(match_cooking(book, FurnaceKind::BlastFurnace, ore).has_value());
    // A blast furnace refuses stone and a smoker refuses ore. Answering
    // "nothing" here is what keeps them different machines.
    CHECK_FALSE(match_cooking(book, FurnaceKind::BlastFurnace, cobble).has_value());
    CHECK_FALSE(match_cooking(book, FurnaceKind::Smoker, ore).has_value());
    REQUIRE(match_cooking(book, FurnaceKind::Smoker, beef).has_value());
    REQUIRE(match_cooking(book, FurnaceKind::Furnace, beef).has_value());
}

TEST_CASE("a whole smelt, tick by tick", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    FurnaceSlots slots;
    slots.input = RecipeStack{item_of("minecraft:iron_ore"), 1};
    slots.fuel  = RecipeStack{item_of("minecraft:coal"), 1};
    FurnaceState state;

    // Tick one lights the furnace and does not spend a tick of its fuel: the
    // 1600 measured on the real server is the count of ticks the block is lit,
    // and one off here is one off for every fuel in the game.
    const FurnaceTick first = furnace_tick(book, FurnaceKind::Furnace, slots, state);
    CHECK(first.lit_changed);
    CHECK(state.lit_time == 1600);
    CHECK(state.lit_duration == 1600);
    CHECK(state.cook_time == 1);
    CHECK(slots.fuel.empty());

    // Iron ore takes 200 ticks in a furnace, which the recipe file says and
    // this does not restate: cook_total came from the matched recipe.
    CHECK(state.cook_total == 200);

    for (int tick = 2; tick <= 200; ++tick) {
        const FurnaceTick step = furnace_tick(book, FurnaceKind::Furnace, slots, state);
        if (tick < 200) {
            CHECK_FALSE(step.produced);
        } else {
            CHECK(step.produced);
        }
    }
    CHECK(item_name(slots.output.item) == "minecraft:iron_ingot");
    CHECK(slots.output.count == 1);
    CHECK(slots.input.empty());
    // One ingot is worth 0.7, and the furnace holds it until someone takes it.
    CHECK(state.stored_experience > 0.69F);
    CHECK(state.stored_experience < 0.71F);

    // 199 ticks of fuel spent, not 200: the tick the furnace lights is a tick
    // it also cooks, and its counter is set rather than spent. That is the
    // measured behaviour, and it is what makes one coal smelt exactly eight.
    CHECK(state.lit_time == 1600 - 199);
    for (int tick = 0; tick < 1401; ++tick) {
        (void)furnace_tick(book, FurnaceKind::Furnace, slots, state);
    }
    CHECK(state.lit_time == 0);
    CHECK_FALSE(state.lit());
}

TEST_CASE("one coal smelts eight items and no more", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    // 1600 ticks of coal over 200 ticks an item is exactly eight, with nothing
    // left over. Feeding it nine ores and getting nine back would mean the
    // furnace is burning fuel it does not have.
    FurnaceSlots slots;
    slots.input = RecipeStack{item_of("minecraft:iron_ore"), 16};
    slots.fuel  = RecipeStack{item_of("minecraft:coal"), 1};
    FurnaceState state;

    int produced = 0;
    for (int tick = 0; tick < 4000; ++tick) {
        if (furnace_tick(book, FurnaceKind::Furnace, slots, state).produced) {
            ++produced;
        }
    }
    CHECK(produced == 8);
    CHECK(slots.output.count == 8);
    CHECK(slots.input.count == 8);
}

TEST_CASE("a lava bucket leaves its bucket in the fuel slot", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    FurnaceSlots slots;
    slots.input = RecipeStack{item_of("minecraft:iron_ore"), 1};
    slots.fuel  = RecipeStack{item_of("minecraft:lava_bucket"), 1};
    FurnaceState state;

    (void)furnace_tick(book, FurnaceKind::Furnace, slots, state);
    CHECK(state.lit_time == 20000);
    REQUIRE_FALSE(slots.fuel.empty());
    CHECK(item_name(slots.fuel.item) == "minecraft:bucket");
    CHECK(slots.fuel.count == 1);
}

TEST_CASE("a furnace with nothing to cook stays dark", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    FurnaceSlots slots;
    slots.fuel = RecipeStack{item_of("minecraft:coal"), 1};
    FurnaceState state;

    for (int tick = 0; tick < 40; ++tick) {
        (void)furnace_tick(book, FurnaceKind::Furnace, slots, state);
    }
    // Fuel untouched: this is why a fuel's burn time cannot be read out of a
    // furnace with an empty input slot, and why the measurement rig had to load
    // three different inputs for three furnaces.
    CHECK_FALSE(state.lit());
    CHECK(slots.fuel.count == 1);
}

TEST_CASE("progress falls back when the fire goes out", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    FurnaceSlots slots;
    slots.input = RecipeStack{item_of("minecraft:iron_ore"), 1};
    slots.fuel  = RecipeStack{item_of("minecraft:bamboo"), 1};  // 50 ticks
    FurnaceState state;

    // Fifty lit ticks, and the first of them is the tick that lights it: the
    // counter is set there, not spent, so fifty ticks of bamboo buy fifty
    // ticks of cooking and not forty-nine.
    for (int tick = 0; tick < 50; ++tick) {
        (void)furnace_tick(book, FurnaceKind::Furnace, slots, state);
    }
    CHECK(state.cook_time == 50);
    CHECK(state.lit_time == 1);
    CHECK(state.lit());

    (void)furnace_tick(book, FurnaceKind::Furnace, slots, state);
    CHECK_FALSE(state.lit());
    // Two ticks of progress lost per dark tick, so the first dark tick already
    // takes it from fifty to forty-eight. Without this a player could cook
    // anything on a handful of sticks fed in one at a time.
    CHECK(state.cook_time == 48);

    for (int tick = 0; tick < 24; ++tick) {
        (void)furnace_tick(book, FurnaceKind::Furnace, slots, state);
    }
    CHECK(state.cook_time == 0);
}

TEST_CASE("a blast furnace is twice as fast and burns twice as much", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    FurnaceSlots slots;
    slots.input = RecipeStack{item_of("minecraft:iron_ore"), 8};
    slots.fuel  = RecipeStack{item_of("minecraft:coal"), 1};
    FurnaceState state;

    // Iron ore takes 100 ticks in a blast furnace, which the recipe file says.
    // Read on the first tick: once the input runs out there is no recipe left
    // to ask, and the total goes back to zero rather than lying.
    (void)furnace_tick(book, FurnaceKind::BlastFurnace, slots, state);
    CHECK(state.cook_total == 100);
    CHECK(state.lit_time == 800);

    int produced = 0;
    for (int tick = 0; tick < 2000; ++tick) {
        if (furnace_tick(book, FurnaceKind::BlastFurnace, slots, state).produced) {
            ++produced;
        }
    }
    // 800 ticks of coal over 100 ticks an item: eight again, in half the time.
    CHECK(produced == 8);
}

TEST_CASE("a stonecutter offers every cut of a block", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    const auto options = stonecutting_options(book, item_of("minecraft:stone"));
    // Stone cuts seven ways in 1.20.1: slab, stairs, bricks, and the brick
    // slab, stairs, wall and chiselled form. The count is what the datapack
    // says, and naming it here makes a version bump visible instead of silent.
    CHECK(options.size() == 7);
    CHECK(stonecutting_options(book, item_of("minecraft:stick")).empty());
}

TEST_CASE("smithing upgrades a diamond pickaxe", "[smelting]") {
    const RecipeBook& book = *loaded().book;

    const auto made = match_smithing(book, item_of("minecraft:netherite_upgrade_smithing_template"),
                                     item_of("minecraft:diamond_pickaxe"),
                                     item_of("minecraft:netherite_ingot"));
    REQUIRE(made.has_value());
    const auto result = book.result(*made);
    REQUIRE(result.has_value());
    CHECK(item_name(result->item) == "minecraft:netherite_pickaxe");

    // The three slots are not interchangeable: putting the ingot where the
    // template goes is not a recipe.
    CHECK_FALSE(match_smithing(book, item_of("minecraft:netherite_ingot"),
                               item_of("minecraft:diamond_pickaxe"),
                               item_of("minecraft:netherite_upgrade_smithing_template"))
                    .has_value());
}
