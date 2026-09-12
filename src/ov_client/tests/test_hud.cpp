// The HUD's rules that decide geometry and timing, without a device.
//
// Every expected number here comes from outside our code: java.util.Random's
// values from the JDK itself (a throwaway program run with `java R.java`), the
// sprite cells from the pack's `icons.png` (scripts/measure_gui_sprites.py),
// the rows, the blink and the wave from the real 1.20.1 client's own numbers
// and captures (scripts/measure_hud.py, facts.txt; docs/provenance/hud.md).

#include "ov/client/boss_bar_view.hpp"
#include "ov/client/container_screen.hpp"
#include "ov/client/debug_overlay.hpp"
#include "ov/client/hud.hpp"
#include "ov/client/scoreboard_view.hpp"
#include "ov/client/status_effects.hpp"
#include "ov/client/tab_list_view.hpp"
#include "ov/render/language.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace ov;
using namespace ov::client;

TEST_CASE("JavaRandom draws what the JDK draws", "[hud]") {
    // new java.util.Random(0L).nextInt(2) × 8, from the JDK.
    JavaRandom zero(0);
    for (const i32 expected : {1, 1, 0, 1, 1, 0, 1, 0}) {
        CHECK(zero.next_int(2) == expected);
    }
    // The seed of tick 2236, (long)(2236 * 312871) = 699579556, nextInt(2) × 10.
    JavaRandom tick(699579556);
    for (const i32 expected : {0, 1, 0, 0, 1, 0, 1, 1, 1, 0}) {
        CHECK(tick.next_int(2) == expected);
    }
    // A bound that is not a power of two takes the rejection loop.
    JavaRandom three(123456789);
    for (const i32 expected : {1, 0, 2, 1, 0, 1}) {
        CHECK(three.next_int(3) == expected);
    }
}

TEST_CASE("heart sprites are the sheet's own cells", "[hud]") {
    // measure_gui_sprites.py, row y = 0 of icons.png.
    CHECK(heart_sprite_x(true, HeartKind::Normal, false, false, false) == 16.0F);
    CHECK(heart_sprite_x(true, HeartKind::Normal, false, false, true) == 25.0F);
    CHECK(heart_sprite_x(false, HeartKind::Normal, false, false, false) == 52.0F);
    CHECK(heart_sprite_x(false, HeartKind::Normal, false, true, false) == 61.0F);
    CHECK(heart_sprite_x(false, HeartKind::Normal, false, false, true) == 70.0F);
    CHECK(heart_sprite_x(false, HeartKind::Normal, false, true, true) == 79.0F);
    CHECK(heart_sprite_x(false, HeartKind::Poisoned, false, false, false) == 88.0F);
    CHECK(heart_sprite_x(false, HeartKind::Poisoned, false, false, true) == 106.0F);
    CHECK(heart_sprite_x(false, HeartKind::Withered, false, true, false) == 133.0F);
    // Absorbing and frozen hearts have no blinking cells.
    CHECK(heart_sprite_x(false, HeartKind::Normal, true, false, true) == 160.0F);
    CHECK(heart_sprite_x(false, HeartKind::Normal, true, true, false) == 169.0F);
    CHECK(heart_sprite_x(false, HeartKind::Frozen, false, false, true) == 178.0F);
    CHECK(heart_sprite_x(false, HeartKind::Frozen, false, true, false) == 187.0F);
}

TEST_CASE("heart rows as the real client laid them out", "[hud]") {
    // 04-hurt: 13 of 20, one row.
    const HeartLayout plain = heart_layout(20.0F, 13.0F, 13, 0.0F);
    CHECK(plain.health_hearts == 10);
    CHECK(plain.absorption_hearts == 0);
    CHECK(plain.rows == 1);
    // 06-absorption: 8 absorption, four gold hearts on a second row ten
    // pixels up (y 431), the armour a row higher still (y 421).
    const HeartLayout gold = heart_layout(20.0F, 13.0F, 13, 8.0F);
    CHECK(gold.absorption_hearts == 4);
    CHECK(gold.rows == 2);
    CHECK(gold.row_height == 10);
    // Three rows squeeze to nine, and never below three.
    CHECK(heart_layout(20.0F, 20.0F, 20, 40.0F).row_height == 9);
    CHECK(heart_layout(1024.0F, 20.0F, 20, 0.0F).row_height == 3);
    // A health shown blinking above the maximum keeps its hearts.
    CHECK(heart_layout(20.0F, 13.0F, 26, 0.0F).health_hearts == 13);
}

TEST_CASE("the regeneration wave runs over max health plus five", "[hud]") {
    CHECK(regeneration_heart(951, 20.0F) == 1);
    CHECK(regeneration_heart(924, 20.0F) == 24);
    CHECK(regeneration_heart(950, 20.0F) == 0);
    CHECK(regeneration_heart(1000, 30.0F) == 0);
}

TEST_CASE("the hearts blink as the real client's timers said", "[hud]") {
    // 03-hurt-blink: seven damage at tick 566 → healthBlinkTime 586, and the
    // health shown stays at 20 for a real second.
    HealthBlink blink;
    blink.update(20, 500, 0);
    blink.update(13, 566, 10'000);
    CHECK(blink.blink_until() == 586);
    CHECK(blink.display_health() == 20);
    CHECK_FALSE(blink.blinking(568));  // (586 − 568) / 3 = 6, even
    CHECK(blink.blinking(577));        // 9 / 3 = 3, odd
    CHECK_FALSE(blink.blinking(586));
    blink.update(13, 590, 10'900);
    CHECK(blink.display_health() == 20);
    blink.update(13, 600, 11'001);
    CHECK(blink.display_health() == 13);
    // A rise blinks ten ticks.
    blink.update(20, 700, 12'000);
    CHECK(blink.blink_until() == 710);
}

TEST_CASE("effects: rows, blink, order", "[hud]") {
    CHECK(effect_is_beneficial("minecraft:speed"));
    CHECK(effect_is_beneficial("minecraft:luck"));
    CHECK_FALSE(effect_is_beneficial("minecraft:slowness"));
    CHECK_FALSE(effect_is_beneficial("minecraft:glowing"));  // neutral: the other row
    CHECK_FALSE(effect_is_beneficial("minecraft:bad_omen"));

    CHECK(effect_icon_alpha(201, false) == 1.0F);
    CHECK(effect_icon_alpha(-1, false) == 1.0F);
    CHECK(effect_icon_alpha(100, true) == 1.0F);
    CHECK(std::abs(effect_icon_alpha(150, false) - 0.575F) < 1e-4F);
    CHECK(std::abs(effect_icon_alpha(135, false) - 0.4F) < 1e-4F);

    std::vector<HudEffect> effects{
        {"minecraft:luck", -1, 0, false, true, true, 1},
        {"minecraft:night_vision", 150, 0, false, true, true, 2},
        {"minecraft:strength", 568, 0, false, true, true, 3},
        {"minecraft:speed", 567, 1, false, true, true, 4},
    };
    sort_effects(effects);
    // The real client, 11-effects, right to left: luck (infinite) first.
    CHECK(effects[0].name == "minecraft:luck");
    CHECK(effects[1].name == "minecraft:strength");
    CHECK(effects[2].name == "minecraft:speed");
    CHECK(effects[3].name == "minecraft:night_vision");
}

TEST_CASE("a boss bar slides over a tenth of a second", "[hud]") {
    render::Language language;
    BossBarView      view;
    netclient::ClientEvents::BossBarChange add{1, 2, 0, 0, R"({"text":"A"})", 0.5F, 2, 1};
    view.apply(add, 0, language);
    REQUIRE(view.bars().size() == 1);
    CHECK(view.bars()[0].progress(0) == 0.5F);
    netclient::ClientEvents::BossBarChange health{1, 2, 2, 0, "", 1.0F, 0, 0};
    view.apply(health, 1000, language);
    CHECK(view.bars()[0].progress(1000) == 0.5F);
    CHECK(std::abs(view.bars()[0].progress(1050) - 0.75F) < 1e-6F);
    CHECK(view.bars()[0].progress(1100) == 1.0F);
    netclient::ClientEvents::BossBarChange second{3, 4, 0, 0, R"({"text":"B"})", 0.0F, 0, 0};
    view.apply(second, 1200, language);
    CHECK(view.bars().size() == 2);
    CHECK(view.bars()[1].most == 3);  // in the order they were added
    netclient::ClientEvents::BossBarChange remove{1, 2, 1, 0, "", 0.0F, 0, 0};
    view.apply(remove, 1300, language);
    REQUIRE(view.bars().size() == 1);
    CHECK(view.bars()[0].most == 3);
}

TEST_CASE("F3's lines sit on the real client's line numbers", "[hud][f3]") {
    // The real client at (-3.5, -60, 7.5), facing south, looking up (19-f3).
    DebugInfo info;
    info.x            = -3.5;
    info.y            = -60.0;
    info.z            = 7.5;
    info.yaw          = 0.0F;
    info.pitch        = -90.0F;
    info.sky_light    = 15;
    info.block_light  = 0;
    info.biome        = "minecraft:plains";
    info.memory_used  = 689ULL * 1024 * 1024;
    info.memory_total = 1504ULL * 1024 * 1024;
    const auto left  = debug_left_lines(info);
    const auto right = debug_right_lines(info);
    REQUIRE(left.size() == 19);
    CHECK(left[0] == "Minecraft 1.20.1 (1.20.1/vanilla)");
    CHECK(left[7] == "minecraft:overworld");
    CHECK(left[8].empty());
    CHECK(left[9] == "XYZ: -3.500 / -60.00000 / 7.500");
    CHECK(left[10] == "Block: -4 -60 7 [12 4 7]");
    CHECK(left[11] == "Chunk: -1 -4 0 [31 0 in r.-1.0.mca]");
    CHECK(left[12] == "Facing: south (Towards positive Z) (0.0 / -90.0)");
    CHECK(left[13] == "Client Light: 15 (15 sky, 0 block)");
    CHECK(left[16] == "Biome: minecraft:plains");
    CHECK(left[17].empty());  // Local Difficulty: not known here
    REQUIRE(right.size() == 10);
    CHECK(right[1] == "Mem:  45% 689/1504MB");
    CHECK(right[0].empty());  // no JVM
}

TEST_CASE("the added windows sit on their textures' slot squares", "[hud][screens]") {
    // Corners from the pack's own textures (the `slots` report): hopper (44,20)
    // step 18 with the player's rows at 51 and 109; dispenser (62,17) 3×3;
    // brewing stand bottles, ingredient, blaze powder; each followed by the
    // player's 36.
    const auto hopper = ContainerScreen::from_menu(15, 1, "Hopper");
    REQUIRE(hopper.has_value());
    CHECK(hopper->slot_count() == 5 + 36);
    CHECK(hopper->height() == 133.0F);
    CHECK(hopper->slots()[4].x == 116.0F);
    CHECK(hopper->slots()[5].y == 51.0F);
    CHECK(hopper->slots()[40].y == 109.0F);
    const auto dispenser = ContainerScreen::from_menu(6, 1, "Dispenser");
    REQUIRE(dispenser.has_value());
    CHECK(dispenser->slot_count() == 9 + 36);
    CHECK(dispenser->slots()[8].x == 98.0F);
    CHECK(dispenser->slots()[8].y == 53.0F);
    const auto brewing = ContainerScreen::from_menu(10, 1, "Brewing Stand");
    REQUIRE(brewing.has_value());
    CHECK(brewing->slots()[1].y == 58.0F);
    CHECK(brewing->slots()[4].x == 17.0F);
    for (const i32 menu : {7, 12, 14, 19}) {
        const auto screen = ContainerScreen::from_menu(menu, 1, "x");
        REQUIRE(screen.has_value());
        CHECK(screen->slots().back().y == 142.0F);  // the hotbar, as every 166-high window
    }
    // Menus no server of ours opens are still refused, not drawn as chests.
    CHECK_FALSE(ContainerScreen::from_menu(23, 1, "Stonecutter").has_value());
    // A donkey with a chest: 2 + 15, five columns of three.
    const ContainerScreen donkey = ContainerScreen::from_horse(2, 17, "Donkey");
    CHECK(donkey.horse_columns() == 5);
    CHECK(donkey.slot_count() == 17 + 36);
    CHECK(donkey.slots()[16].x == 80.0F + 4 * 18.0F);
    CHECK(ContainerScreen::from_horse(2, 2, "Horse").horse_columns() == 0);
    // A llama of strength 3: no saddle slot on screen, a carpet slot, three
    // columns; the hidden saddle slot is still slot 0 on the wire.
    const ContainerScreen llama = ContainerScreen::from_horse(2, 11, "Llama", HorseParts{false, 2});
    CHECK(llama.horse_columns() == 3);
    CHECK(llama.slots()[0].hidden);
    CHECK_FALSE(llama.slots()[1].hidden);
    CHECK(llama.slot_at(854.0F, 480.0F, 339.0F + 8.0F + 1.0F, 157.0F + 18.0F + 1.0F) == nullptr);
}

TEST_CASE("the tab list: latency bars, order, removal", "[hud]") {
    CHECK(TabListView::ping_row(0) == 0);
    CHECK(TabListView::ping_row(149) == 0);
    CHECK(TabListView::ping_row(150) == 1);
    CHECK(TabListView::ping_row(999) == 3);
    CHECK(TabListView::ping_row(1000) == 4);
    CHECK(TabListView::ping_row(-1) == 5);

    TabListView       view;
    ScoreboardView    scoreboard;
    net::PlayerInfoUpdate update;
    update.actions = net::player_info::kAddPlayer | net::player_info::kUpdateGameMode |
                     net::player_info::kUpdateListed;
    net::PlayerInfoEntry spectator;
    spectator.uuid      = net::Uuid{1, 1};
    spectator.name      = "aaron";
    spectator.game_mode = 3;
    spectator.listed    = true;
    net::PlayerInfoEntry bob;
    bob.uuid   = net::Uuid{2, 2};
    bob.name   = "Bob";
    bob.listed = true;
    net::PlayerInfoEntry alice = bob;
    alice.uuid = net::Uuid{3, 3};
    alice.name = "alice";
    net::PlayerInfoEntry hidden = bob;
    hidden.uuid   = net::Uuid{4, 4};
    hidden.name   = "Hidden";
    hidden.listed = false;
    update.entries = {spectator, bob, alice, hidden};
    view.apply(netclient::ClientEvents::TabListEvent{update});
    const auto listed = view.listed(scoreboard);
    REQUIRE(listed.size() == 3);
    CHECK(listed[0]->name == "alice");  // names ignoring case
    CHECK(listed[1]->name == "Bob");
    CHECK(listed[2]->name == "aaron");  // spectators last
    CHECK(view.should_show(true, false, scoreboard));
    CHECK_FALSE(view.should_show(false, false, scoreboard));
    CHECK(view.should_show(true, true, scoreboard));  // three listed on an integrated server

    view.apply(netclient::ClientEvents::TabListEvent{std::vector<net::Uuid>{net::Uuid{2, 2}}});
    CHECK(view.listed(scoreboard).size() == 2);
}
