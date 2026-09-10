// The creative gestures, one measured fact each.
//
// Every expectation here is a line of the facts the real 1.20.1 client gave
// scripts/measure_creative_screen.py (docs/provenance/inventaire-creatif.md
// § 8): what is in hand and in the inventory after the gesture, and what was
// thrown.
#include "ov/client/creative_gestures.hpp"
#include "ov/client/saved_hotbars.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::client;

namespace {

constexpr i32 kLog    = 1;
constexpr i32 kWood   = 2;
constexpr i32 kSword  = 3;
constexpr i32 kHelmet = 4;

CreativeInventory make() {
    CreativeInventory inventory;
    inventory.set_rules([](i32 item) { return item == kSword || item == kHelmet ? 1 : 64; },
                        [](const SlotStack& s) { return s.item == kHelmet ? 5 : -1; });
    return inventory;
}

SlotStack cell(i32 item) {
    return SlotStack{item, 1, {}};
}

}  // namespace

TEST_CASE("a click on a cell takes one, and a second click adds one", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.click_cell(cell(kLog), 0, false, fx);
    CHECK(inv.carried().item == kLog);
    CHECK(inv.carried().count == 1);
    inv.click_cell(cell(kLog), 0, false, fx);
    CHECK(inv.carried().count == 2);
    // Right click on the same cell takes one back.
    inv.click_cell(cell(kLog), 1, false, fx);
    CHECK(inv.carried().count == 1);
    // Nothing reached the server: the cursor is not a slot.
    CHECK(fx.slots.empty());
    CHECK(fx.drops.empty());
}

TEST_CASE("a click on another cell deletes what is in hand", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.click_cell(cell(kLog), 0, false, fx);
    inv.click_cell(cell(kWood), 0, false, fx);
    CHECK(inv.carried().empty());
    inv.click_cell(cell(kLog), 1, false, fx);
    CHECK(inv.carried().count == 1);
    inv.click_cell(cell(kWood), 1, false, fx);
    CHECK(inv.carried().empty());
}

TEST_CASE("middle and shift give a full stack, on the cursor", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.click_cell(cell(kWood), 2, false, fx);
    CHECK(inv.carried().count == 64);
    // Middle with something in hand does nothing.
    inv.click_cell(cell(kLog), 2, false, fx);
    CHECK(inv.carried().item == kWood);

    CreativeInventory shift = make();
    shift.click_cell(cell(kLog), 0, true, fx);
    CHECK(shift.carried().item == kLog);
    CHECK(shift.carried().count == 64);
    CHECK(fx.slots.empty());
}

TEST_CASE("a number key over a cell fills that hotbar slot, only with an empty hand",
          "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.hotbar_key_on_cell(cell(kLog), 3, fx);
    REQUIRE(fx.slots.size() == 1);
    CHECK(fx.slots[0].first == 39);
    CHECK(fx.slots[0].second.count == 64);
    CHECK(inv.slots()[39].item == kLog);

    fx.clear();
    inv.click_cell(cell(kWood), 0, false, fx);
    inv.hotbar_key_on_cell(cell(kWood), 4, fx);
    CHECK(fx.slots.empty());
}

TEST_CASE("Q over a cell throws one, Ctrl+Q a stack, whatever is in hand", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.throw_cell(cell(kLog), false, fx);
    inv.click_cell(cell(kWood), 0, false, fx);
    inv.throw_cell(cell(kLog), true, fx);
    REQUIRE(fx.drops.size() == 2);
    CHECK(fx.drops[0].count == 1);
    CHECK(fx.drops[1].count == 64);
    CHECK(inv.carried().item == kWood);
}

TEST_CASE("the player's slots follow the container rules", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.click_cell(cell(kWood), 2, false, fx);  // 64 wood in hand
    // Right click on an empty slot places one.
    inv.click_slot(41, 1, false, CreativePage::Items, fx);
    CHECK(inv.slots()[41].count == 1);
    CHECK(inv.carried().count == 63);
    // Left click merges.
    inv.click_slot(41, 0, false, CreativePage::Items, fx);
    CHECK(inv.slots()[41].count == 64);
    CHECK(inv.carried().empty());
    // Right click with an empty hand takes the larger half.
    inv.click_slot(41, 1, false, CreativePage::Items, fx);
    CHECK(inv.carried().count == 32);
    CHECK(inv.slots()[41].count == 32);
    // A different item swaps.
    inv.carried() = SlotStack{kLog, 1, {}};
    inv.click_slot(41, 0, false, CreativePage::Items, fx);
    CHECK(inv.slots()[41].item == kLog);
    CHECK(inv.carried().item == kWood);
    CHECK(inv.carried().count == 32);
    // Every slot change became a packet, in order.
    CHECK(fx.slots.back().first == 41);
    CHECK(fx.slots.back().second.item == kLog);
}

TEST_CASE("shift on the hotbar row of a category page deletes the stack", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.slots()[36] = SlotStack{kLog, 5, {}};
    inv.click_slot(36, 0, true, CreativePage::Items, fx);
    CHECK(inv.slots()[36].empty());
    REQUIRE(fx.slots.size() == 1);
    CHECK(fx.slots[0].second.empty());
}

TEST_CASE("outside the panel: left throws the stack, right throws one", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.carried() = SlotStack{kWood, 64, {}};
    inv.click_outside(1, fx);
    CHECK(inv.carried().count == 63);
    inv.click_outside(0, fx);
    CHECK(inv.carried().empty());
    REQUIRE(fx.drops.size() == 2);
    CHECK(fx.drops[0].count == 1);
    CHECK(fx.drops[1].count == 63);
}

TEST_CASE("the destroy slot deletes the hand; shift empties everything", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.carried() = SlotStack{kWood, 64, {}};
    inv.click_destroy(false, fx);
    CHECK(inv.carried().empty());
    CHECK(fx.slots.empty());

    inv.slots()[9]  = SlotStack{kLog, 3, {}};
    inv.slots()[40] = SlotStack{kWood, 3, {}};
    inv.slots()[5]  = SlotStack{kHelmet, 1, {}};
    inv.click_destroy(true, fx);
    CHECK(inv.slots()[9].empty());
    CHECK(inv.slots()[40].empty());
    CHECK(inv.slots()[5].empty());
    CHECK(fx.slots.size() == 3);
}

TEST_CASE("armour slots take only what fits them, one piece", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.carried() = SlotStack{kLog, 64, {}};
    inv.click_slot(5, 0, false, CreativePage::Survival, fx);
    CHECK(inv.slots()[5].empty());
    CHECK(inv.carried().count == 64);
    inv.carried() = SlotStack{kHelmet, 1, {}};
    inv.click_slot(5, 0, false, CreativePage::Survival, fx);
    CHECK(inv.slots()[5].item == kHelmet);
    CHECK(inv.carried().empty());
    CHECK_FALSE(inv.may_place(6, SlotStack{kHelmet, 1, {}}));
}

TEST_CASE("a number key over a player slot swaps with the hotbar", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.slots()[12] = SlotStack{kLog, 7, {}};
    inv.slots()[38] = SlotStack{kWood, 2, {}};
    inv.hotbar_key_on_slot(12, 2, fx);
    CHECK(inv.slots()[12].item == kWood);
    CHECK(inv.slots()[38].item == kLog);
    CHECK(fx.slots.size() == 2);
}

TEST_CASE("Q over a player slot throws one, Ctrl+Q all of it", "[creative][gesture]") {
    CreativeInventory inv = make();
    CreativeEffects   fx;
    inv.slots()[38] = SlotStack{kLog, 64, {}};
    inv.throw_slot(38, false, fx);
    CHECK(inv.slots()[38].count == 63);
    inv.throw_slot(38, true, fx);
    CHECK(inv.slots()[38].empty());
    REQUIRE(fx.drops.size() == 2);
    CHECK(fx.drops[1].count == 63);
}

TEST_CASE("saved hotbars round-trip through vanilla's file layout", "[creative][hotbars]") {
    SavedHotbars saved;
    CHECK(saved.row_empty(3));
    SavedHotbars::Row row{};
    row[0] = SavedStack{"minecraft:stone", 64, {}};
    row[8] = SavedStack{"minecraft:diamond_sword", 1, {}};
    saved.set_row(3, row);

    const auto bytes = saved.serialise();
    auto       back  = SavedHotbars::parse(bytes);
    REQUIRE(back.has_value());
    CHECK_FALSE(back->row_empty(3));
    CHECK(back->row(3)[0].item == "minecraft:stone");
    CHECK(back->row(3)[0].count == 64);
    CHECK(back->row(3)[8].item == "minecraft:diamond_sword");
    CHECK(back->row(3)[1].empty());
    CHECK(back->row_empty(0));

    CHECK_FALSE(SavedHotbars::parse(std::vector<u8>{1, 2, 3}).has_value());
}
