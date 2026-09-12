// Window 0, and the day's own clock.
//
// Both were bugs found by wiring rather than by reasoning, so both are pinned
// here: the click the server used to drop on the floor, and a sky-darkening
// curve that looked plausible and was wrong at every hour but two.
#include "../src/natural_spawning.hpp"
#include "../src/player_inventory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

using namespace ov;
using namespace ov::server;

namespace {

struct Loaded {
    std::optional<registry::Registries>  registries;
    std::optional<gameplay::RecipeBook>  book;
    std::optional<registry::RegistryId>  items;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        if (auto regs = registry::Registries::load(path)) {
            out.registries = std::move(*regs);
            out.book.emplace(*out.registries);
            out.items = out.registries->find("minecraft:item");
        }
        return out;
    }();
    return state;
}

[[nodiscard]] net::ItemStack item(std::string_view name, i8 count) {
    const auto id = loaded().registries->protocol_id(*loaded().items, name);
    REQUIRE(id.has_value());
    return net::ItemStack{static_cast<i32>(*id), count, {}};
}

[[nodiscard]] bool holds(const net::ItemStack& stack, std::string_view name, i8 count) {
    return !stack.empty() && stack.item_id == item(name, 1).item_id && stack.count == count;
}

/// A player's 46 slots, as the protocol numbers them.
using Slots = std::array<net::ItemStack, kPlayerWindowSlots>;

[[nodiscard]] net::ContainerClick click(i16 slot, i8 button, i32 mode) {
    net::ContainerClick out;
    out.window_id = 0;
    out.slot      = slot;
    out.button    = button;
    out.mode      = mode;
    return out;
}

}  // namespace

TEST_CASE("a stack moves inside the player's own window", "[inventory][window0]") {
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;

    // Sixty-four stone in the first hotbar slot.
    slots[kHotbarFirst] = net::ItemStack{1, 64, {}};

    // Left-click picks it up whole.
    auto out = apply_player_click(nullptr, nullptr, click(kHotbarFirst, 0, 0), slots, carried, drag);
    REQUIRE(out.handled);
    REQUIRE(carried.item_id == 1);
    REQUIRE(carried.count == 64);
    REQUIRE(slots[kHotbarFirst].empty());

    // And a second one puts it down in the backpack. This is the whole of the
    // bug: before window 0 was handled, both of these clicks were dropped and
    // the stack never moved.
    out = apply_player_click(nullptr, nullptr, click(kBackpackFirst, 0, 0), slots, carried, drag);
    REQUIRE(out.handled);
    REQUIRE(carried.empty());
    REQUIRE(slots[kBackpackFirst].item_id == 1);
    REQUIRE(slots[kBackpackFirst].count == 64);
}

TEST_CASE("a right click takes half, rounding up", "[inventory][window0]") {
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;
    slots[10] = net::ItemStack{1, 7, {}};

    // Seven splits into four and three, not three and four: vanilla rounds the
    // cursor's half **up**, and rounding down loses an item on every odd stack.
    const auto out = apply_player_click(nullptr, nullptr, click(10, 1, 0), slots, carried, drag);
    REQUIRE(out.handled);
    REQUIRE(carried.count == 4);
    REQUIRE(slots[10].count == 3);
}

TEST_CASE("shift-click trades between the hotbar and the backpack",
          "[inventory][window0]") {
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;
    slots[kHotbarFirst] = net::ItemStack{1, 20, {}};

    auto out = apply_player_click(nullptr, nullptr, click(kHotbarFirst, 0, 1), slots, carried, drag);
    REQUIRE(out.handled);
    REQUIRE(slots[kHotbarFirst].empty());
    REQUIRE(slots[kBackpackFirst].count == 20);

    // And back the other way.
    out = apply_player_click(nullptr, nullptr, click(kBackpackFirst, 0, 1), slots, carried, drag);
    REQUIRE(out.handled);
    REQUIRE(slots[kBackpackFirst].empty());
    REQUIRE(slots[kHotbarFirst].count == 20);
}

TEST_CASE("the craft result slot is never written to", "[inventory][window0]") {
    Slots          slots{};
    net::ItemStack carried{1, 64, {}};
    DragState      drag;

    // Slot 0 shows what the grid makes; it holds nothing of its own. A click
    // that put the cursor down there would let a player duplicate whatever they
    // last crafted, so it is refused rather than treated as storage.
    const auto out =
        apply_player_click(nullptr, nullptr, click(kCraftResultSlot, 0, 0), slots, carried, drag);
    REQUIRE(carried.count == 64);
    REQUIRE(slots[kCraftResultSlot].empty());
    (void)out;
}

// ── The 2x2 against the real server ─────────────────────────────────────────
//
// `scripts/measure_window0.py`: a survival probe clicks in window 0 of a real
// 1.20.1 server, and each case below is one of its readings.

TEST_CASE("the 2x2 answers each click on its result as the real server does",
          "[inventory][window0][parity]") {
    if (!loaded().book) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;
    slots[kCraftGridFirst] = item("minecraft:oak_log", 4);
    const auto press       = [&](i8 button, i32 mode) {
        return apply_player_click(&*loaded().registries, &*loaded().book,
                                        click(kCraftResultSlot, button, mode), slots, carried, drag);
    };

    SECTION("shift-click: every craft, the inventory filled from its end") {
        (void)press(0, 1);
        CHECK(holds(slots[44], "minecraft:oak_planks", 16));
        CHECK(slots[kCraftGridFirst].empty());
    }
    SECTION("a click, left or right: one craft on the cursor") {
        (void)press(1, 0);
        CHECK(holds(carried, "minecraft:oak_planks", 4));
        CHECK(holds(slots[kCraftGridFirst], "minecraft:oak_log", 3));
    }
    SECTION("a number key onto an empty hotbar slot: one craft there") {
        (void)press(2, 2);
        CHECK(holds(slots[kHotbarFirst + 2], "minecraft:oak_planks", 4));
        CHECK(holds(slots[kCraftGridFirst], "minecraft:oak_log", 3));
        CHECK(carried.empty());
    }
    SECTION("a number key onto an occupied one: nothing") {
        slots[kHotbarFirst + 2] = item("minecraft:cobblestone", 1);
        (void)press(2, 2);
        CHECK(holds(slots[kHotbarFirst + 2], "minecraft:cobblestone", 1));
        CHECK(holds(slots[kCraftGridFirst], "minecraft:oak_log", 4));
    }
    SECTION("a throw, either button: exactly one craft, whole") {
        const PlayerClickOutcome one = press(0, 4);
        REQUIRE(one.dropped.size() == 1);
        CHECK(holds(one.dropped[0], "minecraft:oak_planks", 4));
        const PlayerClickOutcome all = press(1, 4);
        REQUIRE(all.dropped.size() == 1);
        CHECK(holds(all.dropped[0], "minecraft:oak_planks", 4));
        CHECK(holds(slots[kCraftGridFirst], "minecraft:oak_log", 2));
    }
}

TEST_CASE("a honey bottle's glass bottle goes back to the grid or the hotbar",
          "[inventory][window0][parity]") {
    if (!loaded().book) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;
    slots[kCraftGridFirst] = item("minecraft:honey_bottle", 2);

    SECTION("one craft: the cell still holds honey, so the bottle goes to hotbar 0") {
        (void)apply_player_click(&*loaded().registries, &*loaded().book,
                                 click(kCraftResultSlot, 0, 0), slots, carried, drag);
        CHECK(holds(carried, "minecraft:sugar", 3));
        CHECK(holds(slots[kCraftGridFirst], "minecraft:honey_bottle", 1));
        CHECK(holds(slots[kHotbarFirst], "minecraft:glass_bottle", 1));
    }
    SECTION("shift-click: the last bottle stays in the emptied cell") {
        (void)apply_player_click(&*loaded().registries, &*loaded().book,
                                 click(kCraftResultSlot, 0, 1), slots, carried, drag);
        CHECK(holds(slots[44], "minecraft:sugar", 6));
        CHECK(holds(slots[kCraftGridFirst], "minecraft:glass_bottle", 1));
        CHECK(holds(slots[kHotbarFirst], "minecraft:glass_bottle", 1));
    }
}

TEST_CASE("closing window 0 gives back the cursor, then the grid", "[inventory][window0][parity]") {
    if (!loaded().registries) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    Slots          slots{};
    net::ItemStack carried = item("minecraft:dirt", 5);
    slots[kCraftGridFirst] = item("minecraft:oak_log", 4);
    const std::vector<net::ItemStack> thrown =
        close_player_window(&*loaded().registries, slots, carried, 0);
    CHECK(thrown.empty());
    CHECK(carried.empty());
    CHECK(slots[kCraftGridFirst].empty());
    CHECK(holds(slots[kHotbarFirst], "minecraft:dirt", 5));
    CHECK(holds(slots[kHotbarFirst + 1], "minecraft:oak_log", 4));
}

TEST_CASE("shift-click wears a helmet and holds a shield", "[inventory][window0][parity]") {
    if (!loaded().registries) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;
    slots[kBackpackFirst]     = item("minecraft:iron_helmet", 1);
    slots[kBackpackFirst + 1] = item("minecraft:shield", 1);
    (void)apply_player_click(&*loaded().registries, nullptr, click(kBackpackFirst, 0, 1), slots,
                             carried, drag);
    (void)apply_player_click(&*loaded().registries, nullptr, click(kBackpackFirst + 1, 0, 1),
                             slots, carried, drag);
    CHECK(holds(slots[kArmourFirst], "minecraft:iron_helmet", 1));
    CHECK(holds(slots[kOffhandSlot], "minecraft:shield", 1));
    CHECK(slots[kBackpackFirst].empty());
    CHECK(slots[kBackpackFirst + 1].empty());
}

TEST_CASE("a double click gathers the cursor's item", "[inventory][window0][parity]") {
    if (!loaded().registries) {
        SKIP("data/vanilla/1.20.1/registry.ovpack is not generated");
    }
    Slots          slots{};
    net::ItemStack carried = item("minecraft:dirt", 10);
    DragState      drag;
    slots[kBackpackFirst + 1] = item("minecraft:dirt", 5);
    slots[kHotbarFirst + 3]   = item("minecraft:dirt", 3);
    (void)apply_player_click(&*loaded().registries, nullptr, click(kBackpackFirst, 0, 6), slots,
                             carried, drag);
    CHECK(holds(carried, "minecraft:dirt", 18));
    CHECK(slots[kBackpackFirst + 1].empty());
    CHECK(slots[kHotbarFirst + 3].empty());
}

TEST_CASE("throwing a stack hands it back to be dropped", "[inventory][window0]") {
    Slots          slots{};
    net::ItemStack carried{};
    DragState      drag;
    slots[12] = net::ItemStack{1, 5, {}};

    // Button 1 throws the whole stack, button 0 a single item.
    auto out = apply_player_click(nullptr, nullptr, click(12, 0, 4), slots, carried, drag);
    REQUIRE(out.dropped.size() == 1);
    REQUIRE(out.dropped[0].count == 1);
    REQUIRE(slots[12].count == 4);

    out = apply_player_click(nullptr, nullptr, click(12, 1, 4), slots, carried, drag);
    REQUIRE(out.dropped.size() == 1);
    REQUIRE(out.dropped[0].count == 4);
    REQUIRE(slots[12].empty());
}

TEST_CASE("the sky darkens on vanilla's curve, not on the day fraction",
          "[spawning][light]") {
    // The three hours the shape is pinned by. The first version of this
    // function ran the raw day fraction through one cosine — plausible, and
    // wrong everywhere: it answered 5 at sunrise, which would let monsters
    // spawn on lit grass the moment a server started.
    CHECK(sky_darken_for(0) == 0);       // sunrise
    CHECK(sky_darken_for(6000) == 0);    // noon
    CHECK(sky_darken_for(18000) == 11);  // midnight

    // Full daylight lasts from sunrise to dusk, and dusk is quick: nothing at
    // 12000, everything two thousand ticks later.
    CHECK(sky_darken_for(12000) == 0);
    CHECK(sky_darken_for(14000) == 11);

    // And it is periodic, in both directions.
    CHECK(sky_darken_for(18000 + 24000) == 11);
    CHECK(sky_darken_for(18000 - 24000) == 11);
}
