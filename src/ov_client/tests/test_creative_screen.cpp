// The creative screen's geometry and its page arithmetic.
//
// Everything here is decided without a device, which is the point: where a
// click lands, which row is on screen after a wheel notch, and what a search
// keeps are all facts a unit test can hold. The drawing is judged by a
// screenshot next to the real client; see docs/provenance/inventaire-creatif.md.
#include "ov/client/creative_screen.hpp"

#include "ov/render/asset_source.hpp"

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
      {"id":"minecraft:search","translation_key":"itemGroup.search",
       "row":"TOP","column":6,"type":"SEARCH","aligned_right":true,
       "icon":"minecraft:compass",
       "display":["minecraft:stone","minecraft:oak_planks","minecraft:stone_bricks"]},
      {"id":"minecraft:combat","translation_key":"itemGroup.combat",
       "row":"BOTTOM","column":1,"type":"CATEGORY","aligned_right":false,
       "icon":"minecraft:netherite_sword","display":["minecraft:bow"]},
      {"id":"minecraft:inventory","translation_key":"itemGroup.inventory",
       "row":"BOTTOM","column":6,"type":"INVENTORY","aligned_right":true,
       "icon":"minecraft:chest","display":[]}]})";
}

render::CreativeTabs make_tabs() {
    auto tabs = render::CreativeTabs::parse(fixture_json());
    REQUIRE(tabs.has_value());
    return std::move(*tabs);
}

/// Names the search test looks for. Without a language file every item
/// translates to its own key, which is enough to filter on.
render::Language make_language() {
    render::MemoryAssetSource source;
    source.add("assets/minecraft/lang/en_us.json",
               R"({"block.minecraft.stone":"Stone",
                   "block.minecraft.oak_planks":"Oak Planks",
                   "block.minecraft.stone_bricks":"Stone Bricks",
                   "itemGroup.buildingBlocks":"Building Blocks"})");
    auto language = render::Language::load(source, "en_us");
    REQUIRE(language.has_value());
    return std::move(*language);
}

constexpr f32 kScreenW = 640.0F;
constexpr f32 kScreenH = 480.0F;

}  // namespace

TEST_CASE("the screen opens on building blocks and knows its rows", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);

    CHECK(screen.tab().id == "minecraft:building_blocks");
    CHECK(screen.page().size() == 60);
    // Sixty cells is seven rows of nine (the last one short), and five fit.
    CHECK(screen.scroll_range() == 2);
    CHECK(screen.scroll_row() == 0);
}

TEST_CASE("the wheel moves the page by rows and stops at the ends", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);

    screen.scroll_by(-1.0F);
    CHECK(screen.scroll_row() == 1);
    screen.scroll_by(-1.0F);
    CHECK(screen.scroll_row() == 2);
    screen.scroll_by(-1.0F);
    CHECK(screen.scroll_row() == 2);
    screen.scroll_by(5.0F);
    CHECK(screen.scroll_row() == 0);

    // A page that fits does not scroll, and the handle does not move.
    REQUIRE(screen.select("minecraft:combat"));
    CHECK(screen.scroll_range() == 0);
    CHECK_FALSE(screen.scrollable());
    screen.scroll_by(-3.0F);
    CHECK(screen.scroll_row() == 0);
}

TEST_CASE("a scrolled page shows the cells that follow", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);

    REQUIRE(screen.cell(0) != nullptr);
    CHECK(screen.cell(0)->item == "minecraft:item0");
    CHECK(screen.cell(44)->item == "minecraft:item44");
    screen.scroll_by(-1.0F);
    CHECK(screen.cell(0)->item == "minecraft:item9");
    // The last row is short: 60 cells, so cell 42 of the second page is the
    // 60th and nothing follows it.
    screen.scroll_by(-1.0F);
    CHECK(screen.cell(0)->item == "minecraft:item18");
    CHECK(screen.cell(41)->item == "minecraft:item59");
    CHECK(screen.cell(42) == nullptr);
}

TEST_CASE("the search filters on the translated name", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);

    REQUIRE(screen.select("minecraft:search"));
    CHECK(screen.searching());
    CHECK(screen.page().size() == 3);

    screen.type("stone");
    // Case-insensitive, and on the name rather than the id: "Stone" and
    // "Stone Bricks" match, "Oak Planks" does not.
    CHECK(screen.page().size() == 2);
    CHECK(screen.page()[0]->item == "minecraft:stone");
    CHECK(screen.page()[1]->item == "minecraft:stone_bricks");

    screen.backspace();
    CHECK(screen.query() == "ston");
    CHECK(screen.page().size() == 2);

    screen.type("XYZ");
    CHECK(screen.page().empty());

    // Typing into a page that is not the search tab does nothing.
    REQUIRE(screen.select("minecraft:building_blocks"));
    screen.type("stone");
    CHECK(screen.page().size() == 60);
}

TEST_CASE("a click lands on the cell the pixels say", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);

    const f32 ox = screen.origin_x(kScreenW);
    const f32 oy = screen.origin_y(kScreenH);

    // The first cell's top-left is (9, 18) in the panel, measured out of
    // tab_items.png.
    auto hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + 0.5F, oy + kCellY + 0.5F);
    CHECK(hit.kind == CreativeHit::Cell);
    CHECK(hit.index == 0);

    // The last cell of the top row is the ninth, pitch 18.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + 8 * kCellPitch + 15.0F,
                          oy + kCellY + 15.0F);
    CHECK(hit.index == 8);

    // The gap between two cells belongs to neither.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + kCellSize + 0.5F, oy + kCellY + 1.0F);
    CHECK(hit.kind == CreativeHit::None);

    // The hotbar row is the player's slots 36..44, not cells.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + 2 * kCellPitch + 1.0F,
                          oy + kHotbarY + 1.0F);
    CHECK(hit.kind == CreativeHit::PlayerSlot);
    CHECK(hit.index == 38);

    // The scrollbar groove, x = 175..186 out of the texture.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kScrollX + 1.0F, oy + kScrollY + 40.0F);
    CHECK(hit.kind == CreativeHit::Scrollbar);
}

TEST_CASE("the tab buttons are where the sheet's pitch puts them", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);

    const f32 ox = screen.origin_x(kScreenW);
    const f32 oy = screen.origin_y(kScreenH);

    // Column 0 of the top row hangs above the panel.
    auto hit = screen.hit_test(kScreenW, kScreenH, ox + 1.0F, oy - kTabHeight + kTabOverlap + 1.0F);
    CHECK(hit.kind == CreativeHit::Tab);
    CHECK(tabs.tabs()[static_cast<usize>(hit.index)].id == "minecraft:building_blocks");

    // Column 6 of the top row, at pitch 28: 168..193.
    hit = screen.hit_test(kScreenW, kScreenH, ox + 6 * kTabPitch + 1.0F,
                          oy - kTabHeight + kTabOverlap + 1.0F);
    CHECK(tabs.tabs()[static_cast<usize>(hit.index)].id == "minecraft:search");

    // Column 1 of the bottom row hangs below it.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kTabPitch + 1.0F,
                          oy + kPanelHeight - kTabOverlap + 1.0F);
    CHECK(tabs.tabs()[static_cast<usize>(hit.index)].id == "minecraft:combat");

    // The two-pixel gap between buttons belongs to neither.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kTabWidth + 0.5F,
                          oy - kTabHeight + kTabOverlap + 1.0F);
    CHECK(hit.kind == CreativeHit::None);
}

TEST_CASE("the survival page has the destroy slot and the whole inventory", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    REQUIRE(screen.select("minecraft:inventory"));

    const f32 ox = screen.origin_x(kScreenW);
    const f32 oy = screen.origin_y(kScreenH);

    // (173, 112): the one 16×16 square in tab_inventory.png that is pink
    // rather than slot grey.
    auto hit = screen.hit_test(kScreenW, kScreenH, ox + kDestroyX + 1.0F, oy + kDestroyY + 1.0F);
    CHECK(hit.kind == CreativeHit::Destroy);

    // The three rows of nine are window-0 slots 9..35.
    hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + 1.0F, oy + 54.0F + 1.0F);
    CHECK(hit.kind == CreativeHit::PlayerSlot);
    CHECK(hit.index == 9);
    hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + 8 * kCellPitch + 1.0F,
                          oy + 90.0F + 1.0F);
    CHECK(hit.index == 35);
    hit = screen.hit_test(kScreenW, kScreenH, ox + kCellX + 1.0F, oy + kHotbarY + 1.0F);
    CHECK(hit.index == 36);

    CHECK(screen.background_texture()
          == "minecraft:gui/container/creative_inventory/tab_inventory");
}

TEST_CASE("dragging the handle picks a row", "[creative]") {
    const auto     tabs     = make_tabs();
    const auto     language = make_language();
    CreativeScreen screen(tabs, language);
    const f32      oy = screen.origin_y(kScreenH);

    screen.drag_scroll(kScreenH, oy + kScrollY);
    CHECK(screen.scroll_row() == 0);
    screen.drag_scroll(kScreenH, oy + kScrollY + kScrollHeight);
    CHECK(screen.scroll_row() == 2);
    screen.drag_scroll(kScreenH, oy + kScrollY + (kScrollHeight - kHandleHeight) * 0.5F
                                    + kHandleHeight * 0.5F);
    CHECK(screen.scroll_row() == 1);
}
