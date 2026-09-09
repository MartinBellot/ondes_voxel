// Window 0, and the day's own clock.
//
// Both were bugs found by wiring rather than by reasoning, so both are pinned
// here: the click the server used to drop on the floor, and a sky-darkening
// curve that looked plausible and was wrong at every hour but two.
#include "../src/natural_spawning.hpp"
#include "../src/player_inventory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace ov;
using namespace ov::server;

namespace {

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
