// The creative tab table, on a fixture rather than on the generated file.
//
// The real table is derived from Mojang's jar and is gitignored, so a test
// that opened it would be a test that cannot run in CI. What is testable here
// is the *shape*: the row split, the column order, the two spellings of a
// cell, the base64 of an NBT payload, and the refusal of a tab kind this
// client does not draw. The numbers of the real file are checked by
// scripts/measure_creative_tabs.py --check, which is where they belong.
#include "ov/render/creative_tabs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::render;

namespace {

// "AAA=" is a two-byte payload; a real cell's NBT begins with 0x0A
// (TAG_Compound) and an empty name, which is what CgAA decodes to.
constexpr std::string_view kFixture = R"({
  "version": "1.20.1",
  "op_permissions": true,
  "tabs": [
    {"id":"minecraft:building_blocks","translation_key":"itemGroup.buildingBlocks",
     "row":"TOP","column":0,"type":"CATEGORY","aligned_right":false,
     "icon":"minecraft:bricks",
     "display":["minecraft:oak_planks","minecraft:stone"]},
    {"id":"minecraft:search","translation_key":"itemGroup.search",
     "row":"TOP","column":6,"type":"SEARCH","aligned_right":true,
     "icon":"minecraft:compass",
     "display":["minecraft:stone",
                {"item":"minecraft:potion","nbt":"CgAA"},
                {"item":"minecraft:firework_rocket","count":3}]},
    {"id":"minecraft:combat","translation_key":"itemGroup.combat",
     "row":"BOTTOM","column":1,"type":"CATEGORY","aligned_right":false,
     "icon":"minecraft:netherite_sword",
     "display":["minecraft:bow"]},
    {"id":"minecraft:inventory","translation_key":"itemGroup.inventory",
     "row":"BOTTOM","column":6,"type":"INVENTORY","aligned_right":true,
     "icon":"minecraft:chest","display":[]}
  ]
})";

CreativeTabs parse_ok(std::string_view text) {
    auto tabs = CreativeTabs::parse(text);
    REQUIRE(tabs.has_value());
    return std::move(*tabs);
}

}  // namespace

TEST_CASE("a tab table keeps the game's order and kinds", "[creative]") {
    const CreativeTabs tabs = parse_ok(kFixture);

    CHECK(tabs.version() == "1.20.1");
    CHECK(tabs.op_permissions());
    REQUIRE(tabs.tabs().size() == 4);

    const CreativeTab* blocks = tabs.find("minecraft:building_blocks");
    REQUIRE(blocks != nullptr);
    CHECK(blocks->type == CreativeTabType::Category);
    CHECK(blocks->row == CreativeTabRow::Top);
    CHECK(blocks->column == 0);
    CHECK(blocks->translation_key == "itemGroup.buildingBlocks");
    CHECK(blocks->icon == "minecraft:bricks");
    CHECK_FALSE(blocks->aligned_right);
    REQUIRE(blocks->stacks.size() == 2);
    // Order is the game's, not alphabetical: planks before stone.
    CHECK(blocks->stacks[0].item == "minecraft:oak_planks");
    CHECK(blocks->stacks[1].item == "minecraft:stone");

    CHECK(tabs.find("minecraft:search")->type == CreativeTabType::Search);
    CHECK(tabs.find("minecraft:inventory")->type == CreativeTabType::Inventory);
    CHECK(tabs.find("minecraft:inventory")->aligned_right);
    CHECK(tabs.find("minecraft:nothing") == nullptr);
}

TEST_CASE("the two rows are separate and sorted by column", "[creative]") {
    const CreativeTabs tabs = parse_ok(kFixture);

    const auto top = tabs.row(CreativeTabRow::Top);
    REQUIRE(top.size() == 2);
    CHECK(top[0]->column == 0);
    CHECK(top[1]->column == 6);

    const auto bottom = tabs.row(CreativeTabRow::Bottom);
    REQUIRE(bottom.size() == 2);
    CHECK(bottom[0]->id == "minecraft:combat");
    CHECK(bottom[1]->id == "minecraft:inventory");
}

TEST_CASE("a cell carries its count and its NBT bytes unparsed", "[creative]") {
    const CreativeTabs tabs   = parse_ok(kFixture);
    const CreativeTab* search = tabs.find("minecraft:search");
    REQUIRE(search != nullptr);
    REQUIRE(search->stacks.size() == 3);

    // A plain string is a stack of one with no tag.
    CHECK(search->stacks[0].item == "minecraft:stone");
    CHECK(search->stacks[0].count == 1);
    CHECK(search->stacks[0].nbt.empty());

    // "CgAA" is 0x0A 0x00 0x00: TAG_Compound, an empty name, and the start of
    // the payload — exactly what a Slot carries after the count.
    REQUIRE(search->stacks[1].nbt.size() == 3);
    CHECK(search->stacks[1].nbt[0] == 0x0A);
    CHECK(search->stacks[1].nbt[1] == 0x00);
    CHECK(search->stacks[1].nbt[2] == 0x00);

    CHECK(search->stacks[2].count == 3);
    CHECK(search->stacks[2].nbt.empty());
}

TEST_CASE("only the category tabs count towards the cell total", "[creative]") {
    const CreativeTabs tabs = parse_ok(kFixture);
    // Two in building_blocks and one in combat. The search tab holds the union
    // and would double every item.
    CHECK(tabs.cell_count() == 3);
}

TEST_CASE("a tab kind this client cannot draw is refused, not defaulted", "[creative]") {
    const CreativeTabs tabs = parse_ok(R"({
      "version":"1.20.1","op_permissions":false,
      "tabs":[{"id":"minecraft:mystery","translation_key":"","row":"TOP","column":0,
               "type":"SOMETHING_NEW","aligned_right":false,"icon":"minecraft:stone",
               "display":["minecraft:stone"]}]})");
    CHECK(tabs.tabs().empty());
    CHECK(tabs.cell_count() == 0);
}

TEST_CASE("a document that is not a tab table is an error, not an empty one", "[creative]") {
    CHECK_FALSE(CreativeTabs::parse("not json").has_value());
    CHECK_FALSE(CreativeTabs::parse(R"({"version":"1.20.1"})").has_value());
    CHECK(CreativeTabs::load("no/such/file.json").error() == CreativeTabsError::NotFound);
}
