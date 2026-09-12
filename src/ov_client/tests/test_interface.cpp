#include "ov/client/container_screen.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/hud.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

using namespace ov;
using namespace ov::client;

// Nothing here touches a GPU. Every claim below is about geometry or about a
// number the protocol carries, and both are decidable without a device — which
// is the whole reason the layout is a table of constants rather than something
// the renderer works out as it draws.

TEST_CASE("the automatic gui scale is the largest that still leaves 320x240",
          "[interface]") {
    // Vanilla's rule, and the reason a hotbar is legible on a 4K display and
    // still fits on a small one.
    CHECK(auto_gui_scale(1280, 720) == 3);   // 720/3 = 240 exactly; /4 would be 180
    CHECK(auto_gui_scale(2560, 1440) == 6);  // 1440/6 = 240
    CHECK(auto_gui_scale(1920, 1080) == 4);  // 1080/4 = 270, /5 = 216
    CHECK(auto_gui_scale(640, 480) == 2);
    // Below the minimum there is no honest answer, and one is the floor rather
    // than zero: a scale of zero would divide by it.
    CHECK(auto_gui_scale(320, 200) == 1);
    CHECK(auto_gui_scale(1, 1) == 1);
    // The player's setting caps it.
    CHECK(auto_gui_scale(2560, 1440, 2) == 2);
}

TEST_CASE("the player's window has the slots the protocol says it has",
          "[interface]") {
    const ContainerScreen screen = ContainerScreen::player_inventory();
    // Exactly 46, and `inventory.png` yields exactly 46 slot boxes. The two
    // agreeing is what says the layout and the protocol are the same window.
    REQUIRE(screen.slot_count() == 46);
    CHECK(screen.window_id() == 0);

    const auto& slots = screen.slots();
    // Every corner below was measured out of the texture by
    // scripts/measure_gui_sprites.py.
    CHECK(slots[0].x == 154.0F);   // crafting result
    CHECK(slots[0].y == 28.0F);
    CHECK(slots[1].x == 98.0F);    // 2x2 grid, top left
    CHECK(slots[1].y == 18.0F);
    CHECK(slots[4].x == 116.0F);   // 2x2 grid, bottom right
    CHECK(slots[4].y == 36.0F);
    CHECK(slots[5].x == 8.0F);     // helmet
    CHECK(slots[5].y == 8.0F);
    CHECK(slots[8].y == 62.0F);    // boots
    CHECK(slots[9].x == 8.0F);     // first inventory slot
    CHECK(slots[9].y == 84.0F);
    CHECK(slots[36].y == 142.0F);  // first hotbar slot
    CHECK(slots[45].x == 77.0F);   // off hand
    CHECK(slots[45].y == 62.0F);

    // The indices are the window's own numbering, in order.
    for (usize i = 0; i < slots.size(); ++i) {
        CHECK(slots[i].index == static_cast<i16>(i));
    }
}

TEST_CASE("a chest's size comes from its menu type", "[interface]") {
    struct Case {
        i32   menu;
        usize slots;
        f32   height;
        f32   player_row_y;
        f32   hotbar_y;
    };
    // 114 + rows*18 tall, the player's three rows at 103 + (rows-4)*18, the
    // hotbar at 161 + the same. For six rows that puts them at 139 and 197,
    // which is where generic_54.png's own slot boxes are once the two-part
    // blit has shifted the bottom section by one.
    constexpr std::array kCases{
        Case{0, 9 + 36, 132.0F, 49.0F, 107.0F},   Case{2, 27 + 36, 168.0F, 85.0F, 143.0F},
        Case{5, 54 + 36, 222.0F, 139.0F, 197.0F},
    };
    for (const Case& c : kCases) {
        const auto screen = ContainerScreen::from_menu(c.menu, 3, "Chest");
        REQUIRE(screen.has_value());
        CHECK(screen->kind() == ScreenKind::Chest);
        CHECK(screen->slot_count() == c.slots);
        CHECK(screen->height() == c.height);
        const usize container = c.slots - 36;
        CHECK(screen->slots()[container].y == c.player_row_y);
        CHECK(screen->slots()[c.slots - 9].y == c.hotbar_y);
        CHECK(screen->window_id() == 3);
    }
}

TEST_CASE("a crafting table and a furnace have their own slot lists",
          "[interface]") {
    const auto crafting = ContainerScreen::from_menu(11, 1, "Crafting");
    REQUIRE(crafting.has_value());
    CHECK(crafting->kind() == ScreenKind::CraftingTable);
    // Result, then nine grid slots, then the player's thirty-six.
    CHECK(crafting->slot_count() == 1 + 9 + 36);
    CHECK(crafting->slots()[0].x == 124.0F);
    CHECK(crafting->slots()[0].y == 35.0F);
    CHECK(crafting->slots()[1].x == 30.0F);
    CHECK(crafting->slots()[1].y == 17.0F);

    // Furnace, blast furnace and smoker are the same three slots.
    for (const i32 menu : {9, 13, 21}) {
        const auto furnace = ContainerScreen::from_menu(menu, 1, "Furnace");
        REQUIRE(furnace.has_value());
        CHECK(furnace->kind() == ScreenKind::Furnace);
        CHECK(furnace->slot_count() == 3 + 36);
        CHECK(furnace->slots()[0].x == 56.0F);   // input
        CHECK(furnace->slots()[0].y == 17.0F);
        CHECK(furnace->slots()[1].y == 53.0F);   // fuel
        CHECK(furnace->slots()[2].x == 116.0F);  // output
    }
}

TEST_CASE("a menu shape this client does not draw is refused", "[interface]") {
    // A stonecutter drawn as a chest would put its slots where the player's
    // inventory is, and every click would name a slot that means something else
    // on the server. Refusing is the only safe answer. ── hud ── The anvil (7),
    // brewing stand (10), enchanting table (12) and hopper (15) are drawn now
    // (test_hud.cpp); these are the menus no server of ours opens: beacon,
    // lectern, loom, merchant, smithing, cartography, stonecutter.
    for (const i32 menu : {8, 16, 17, 18, 20, 22, 23, 999}) {
        CHECK_FALSE(ContainerScreen::from_menu(menu, 1, "?").has_value());
    }
}

TEST_CASE("hit testing finds the slot under the pointer", "[interface]") {
    const ContainerScreen screen = ContainerScreen::player_inventory();
    // A 400x300 screen puts a 176x166 window at (112, 67).
    constexpr f32 kWidth  = 400.0F;
    constexpr f32 kHeight = 300.0F;
    CHECK(screen.origin_x(kWidth) == 112.0F);
    CHECK(screen.origin_y(kHeight) == 67.0F);

    const SlotRect* hotbar = screen.slot_at(kWidth, kHeight, 112.0F + 8.0F, 67.0F + 142.0F);
    REQUIRE(hotbar != nullptr);
    CHECK(hotbar->index == 36);

    // The cell is sixteen wide and the pitch is eighteen, so the two pixels
    // between two slots belong to neither. Treating them as part of a slot
    // makes a click at the edge land one slot over.
    CHECK(screen.slot_at(kWidth, kHeight, 112.0F + 23.0F, 67.0F + 142.0F) == hotbar);
    CHECK(screen.slot_at(kWidth, kHeight, 112.0F + 24.0F, 67.0F + 142.0F) == nullptr);
    CHECK(screen.slot_at(kWidth, kHeight, 112.0F + 25.0F, 67.0F + 142.0F) == nullptr);
    const SlotRect* second = screen.slot_at(kWidth, kHeight, 112.0F + 26.0F, 67.0F + 142.0F);
    REQUIRE(second != nullptr);
    CHECK(second->index == 37);
    CHECK(screen.slot_at(kWidth, kHeight, 0.0F, 0.0F) == nullptr);
}

TEST_CASE("a click becomes one of the six modes", "[interface]") {
    // Plain left and right.
    CHECK(ContainerScreen::click(5, 0, false, -1).mode == click_mode::kPickup);
    CHECK(ContainerScreen::click(5, 0, false, -1).button == 0);
    CHECK(ContainerScreen::click(5, 1, false, -1).button == 1);

    // Shift is a different operation, not a modifier on the same one.
    CHECK(ContainerScreen::click(5, 0, true, -1).mode == click_mode::kQuickMove);

    // A number key is mode 2, and the *hotbar index* travels in the button
    // field. Reading it as a mouse button turns "move to slot 3" into a
    // right-click.
    const ClickIntent swap = ContainerScreen::click(5, 0, false, 3);
    CHECK(swap.mode == click_mode::kSwap);
    CHECK(swap.button == 3);
    CHECK(swap.slot == 5);

    // Middle click clones. Sent whatever the game mode: the server decides.
    CHECK(ContainerScreen::click(5, 2, false, -1).mode == click_mode::kClone);

    // A number key wins over the mouse button, because it is the thing that
    // was pressed.
    CHECK(ContainerScreen::click(5, 1, true, 8).mode == click_mode::kSwap);
}

TEST_CASE("armour points add up over the four slots", "[interface]") {
    CHECK(armour_points("minecraft:diamond_chestplate") == 8);
    CHECK(armour_points("minecraft:leather_boots") == 1);
    CHECK(armour_points("minecraft:turtle_helmet") == 2);
    // Anything that is not armour is worth nothing, including an empty name.
    CHECK(armour_points("minecraft:stone") == 0);
    CHECK(armour_points(std::string_view{}) == 0);

    const std::array<ItemStackView, 4> full{
        ItemStackView{"minecraft:diamond_helmet", 1},
        ItemStackView{"minecraft:diamond_chestplate", 1},
        ItemStackView{"minecraft:diamond_leggings", 1},
        ItemStackView{"minecraft:diamond_boots", 1}};
    CHECK(armour_points(full) == 20);

    // An empty slot contributes nothing rather than being skipped by name.
    const std::array<ItemStackView, 4> partial{ItemStackView{},
                                               ItemStackView{"minecraft:iron_chestplate", 1},
                                               ItemStackView{}, ItemStackView{}};
    CHECK(armour_points(partial) == 6);
}
