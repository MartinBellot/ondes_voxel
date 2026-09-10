// The creative screen's geometry and its page arithmetic.
//
// Every position here was measured on the running 1.20.1 client
// (scripts/measure_creative_screen.py asks the game where each tab button and
// slot is); the tests hold the screen to those numbers.
#include "ov/client/creative_screen.hpp"

#include "ov/render/asset_source.hpp"
#include "ov/render/text_component.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::client;
using namespace ov::client::creative_layout;

namespace {

/// Sixty cells: seven rows of nine minus three, so the page scrolls by two.
std::string fixture_json() {
    std::string display;
    for (int i = 0; i < 60; ++i) {
        if (i != 0) {
            display += ',';
        }
        display += "\"minecraft:item" + std::to_string(i) + "\"";
    }
    return R"({"version":"1.20.1","op_permissions":true,"tabs":[
      {"id":"minecraft:building_blocks","translation_key":"itemGroup.buildingBlocks",
       "row":"TOP","column":0,"type":"CATEGORY","aligned_right":false,
       "icon":"minecraft:bricks","display":[)"
         + display + R"(]},
      {"id":"minecraft:redstone_blocks","translation_key":"itemGroup.redstone",
       "row":"TOP","column":4,"type":"CATEGORY","aligned_right":false,
       "icon":"minecraft:redstone","display":["minecraft:redstone"]},
      {"id":"minecraft:hotbar","translation_key":"itemGroup.hotbar",
       "row":"TOP","column":5,"type":"HOTBAR","aligned_right":true,
       "icon":"minecraft:bookshelf","display":[]},
      {"id":"minecraft:search","translation_key":"itemGroup.search",
       "row":"TOP","column":6,"type":"SEARCH","aligned_right":true,
       "icon":"minecraft:compass",
       "display":["minecraft:stone","minecraft:oak_planks","minecraft:stone_bricks",
                  "minecraft:command_block"]},
      {"id":"minecraft:combat","translation_key":"itemGroup.combat",
       "row":"BOTTOM","column":1,"type":"CATEGORY","aligned_right":false,
       "icon":"minecraft:netherite_sword","display":["minecraft:bow"]},
      {"id":"minecraft:op_blocks","translation_key":"itemGroup.op",
       "row":"BOTTOM","column":5,"type":"CATEGORY","aligned_right":false,
       "icon":"minecraft:command_block","display":["minecraft:command_block"]},
      {"id":"minecraft:inventory","translation_key":"itemGroup.inventory",
       "row":"BOTTOM","column":6,"type":"INVENTORY","aligned_right":true,
       "icon":"minecraft:chest","display":[]}]})";
}

render::CreativeTabs make_tabs() {
    auto tabs = render::CreativeTabs::parse(fixture_json());
    REQUIRE(tabs.has_value());
    return std::move(*tabs);
}

render::Language make_language() {
    render::MemoryAssetSource source;
    source.add("assets/minecraft/lang/en_us.json",
               R"({"block.minecraft.stone":"Stone",
                   "block.minecraft.oak_planks":"Oak Planks",
                   "block.minecraft.stone_bricks":"Stone Bricks",
                   "block.minecraft.command_block":"Command Block",
                   "itemGroup.buildingBlocks":"Building Blocks",
                   "inventory.hotbarInfo":"Save hotbar with %1$s+%2$s"})");
    auto language = render::Language::load(source, "en_us");
    REQUIRE(language.has_value());
    return std::move(*language);
}

/// 2560×1440 at scale 3, as the running client reported: 854×480 GUI pixels,
/// the panel at (329, 172).
constexpr f32 kScreenW = 854.0F;
constexpr f32 kScreenH = 480.0F;

}  // namespace

TEST_CASE("the panel is where the running client put it", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    CHECK(screen.origin_x(kScreenW) == 329.0F);
    CHECK(screen.origin_y(kScreenH) == 172.0F);
}

TEST_CASE("the operator tab and its stacks are hidden by default", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    for (const render::CreativeTab* tab : screen.tabs()) {
        CHECK(tab->id != "minecraft:op_blocks");
    }
    REQUIRE(screen.select("minecraft:search"));
    CHECK(screen.page().size() == 3);

    CreativeScreen op(tabs, language, nullptr, CreativeScreenOptions{true});
    CHECK(op.tab_count() == screen.tab_count() + 1);
    REQUIRE(op.select("minecraft:search"));
    CHECK(op.page().size() == 4);
}

TEST_CASE("the wheel moves the page by rows and stops at the ends", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    CHECK(screen.tab().id == "minecraft:building_blocks");
    CHECK(screen.scroll_range() == 2);
    screen.scroll_by(-1.0F);
    CHECK(screen.scroll_row() == 1);
    screen.scroll_by(-1.0F);
    screen.scroll_by(-1.0F);
    CHECK(screen.scroll_row() == 2);
    screen.scroll_by(5.0F);
    CHECK(screen.scroll_row() == 0);
    REQUIRE(screen.cell(44) != nullptr);
    CHECK(screen.cell(44)->item == "minecraft:item44");
}

TEST_CASE("the search is a substring, case-folded, not trimmed", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    REQUIRE(screen.select("minecraft:search"));
    screen.type("Stone");
    CHECK(screen.page().size() == 2);
    screen.backspace();
    CHECK(screen.query() == "Ston");
    // Leading spaces are part of the query: the running client finds nothing
    // for "  stone".
    while (!screen.query().empty()) {
        screen.backspace();
    }
    screen.type("  stone");
    CHECK(screen.page().empty());
    // A colon searches ids.
    while (!screen.query().empty()) {
        screen.backspace();
    }
    screen.type("minecraft:");
    CHECK(screen.page().size() == 3);
    // The box holds fifty characters.
    screen.type(std::string(80, 'x'));
    CHECK(screen.query().size() == 50);
}

TEST_CASE("tab buttons react where the running client says", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    const f32      ox = screen.origin_x(kScreenW);
    const f32      oy = screen.origin_y(kScreenH);

    const auto id_at = [&](f32 x, f32 y) -> std::string {
        const CreativeTarget hit = screen.hit_test(kScreenW, kScreenH, ox + x, oy + y);
        if (hit.kind != CreativeHit::Tab) {
            return "";
        }
        return std::string(screen.tabs()[static_cast<usize>(hit.index)]->id);
    };
    // getTabX: 27 × column; right-aligned tabs from the right edge.
    CHECK(tab_x(4, false) == 108.0F);
    CHECK(tab_x(5, true) == 142.0F);
    CHECK(tab_x(6, true) == 169.0F);
    CHECK(id_at(108.5F, -31.5F) == "minecraft:redstone_blocks");
    CHECK(id_at(142.5F, -1.0F) == "minecraft:hotbar");
    CHECK(id_at(169.5F, -20.0F) == "minecraft:search");
    CHECK(id_at(27.5F, 136.5F) == "minecraft:combat");
    CHECK(id_at(169.5F, 167.0F) == "minecraft:inventory");
    // Between Redstone Blocks (ends at 134) and Saved Hotbars (starts at 142).
    CHECK(id_at(138.0F, -20.0F).empty());
    // The four pixels a button overlaps the panel by belong to the panel.
    CHECK(id_at(1.0F, 1.0F).empty());
}

TEST_CASE("the survival page's slots are the running client's", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    REQUIRE(screen.select("minecraft:inventory"));
    const f32 ox = screen.origin_x(kScreenW);
    const f32 oy = screen.origin_y(kScreenH);
    const auto slot_at = [&](f32 x, f32 y) {
        const CreativeTarget hit = screen.hit_test(kScreenW, kScreenH, ox + x + 1.0F, oy + y + 1.0F);
        return hit.kind == CreativeHit::PlayerSlot ? hit.index : -1;
    };
    // Helmet (inventory 39 = window 5) at (54,6), chestplate (6) at (54,33),
    // leggings (7) at (108,6), boots (8) at (108,33), off hand (45) at (35,20).
    CHECK(slot_at(54, 6) == 5);
    CHECK(slot_at(54, 33) == 6);
    CHECK(slot_at(108, 6) == 7);
    CHECK(slot_at(108, 33) == 8);
    CHECK(slot_at(35, 20) == 45);
    CHECK(slot_at(9, 54) == 9);
    CHECK(slot_at(153, 90) == 35);
    CHECK(slot_at(9, 112) == 36);
    const CreativeTarget destroy =
        screen.hit_test(kScreenW, kScreenH, ox + kDestroyX + 1.0F, oy + kDestroyY + 1.0F);
    CHECK(destroy.kind == CreativeHit::Destroy);
}

TEST_CASE("the saved hotbars page shows a hint on the diagonal", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    SavedHotbars   saved;
    std::array<std::string, 9> keys{"&", "\xC3\x89", "\"", "'", "(", "\xC2\xA7", "\xC3\x88", "!", "\xC3\x87"};
    screen.set_saved_hotbars(&saved, "C", keys);
    REQUIRE(screen.select("minecraft:hotbar"));
    CHECK(screen.page().size() == 81);
    CHECK(screen.scroll_range() == 4);
    REQUIRE(screen.cell(0) != nullptr);
    CHECK(screen.cell(0)->hint);
    CHECK(screen.cell(1) == nullptr);
    REQUIRE(screen.cell(10) != nullptr);
    CHECK(screen.cell(10)->hint);

    std::vector<std::string> lines;
    screen.tooltip(CreativeTarget{CreativeHit::Cell, 0}, {}, lines);
    REQUIRE(lines.size() == 1);
    // Italic white, with the style re-emitted around each argument.
    CHECK(render::strip_formatting(lines[0]) == "Save hotbar with C+&");

    SavedHotbars::Row row{};
    row[2] = SavedStack{"minecraft:stone", 64, {}};
    saved.set_row(0, row);
    screen.refresh_saved_hotbars();
    CHECK(screen.cell(0) == nullptr);
    REQUIRE(screen.cell(2) != nullptr);
    CHECK(screen.cell(2)->item == "minecraft:stone");
}
