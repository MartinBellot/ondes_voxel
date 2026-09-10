// Chat components flattened into `§`-coded lines, and the creative item data
// the real client dumps. The fixtures are shaped exactly like what
// scripts/creative_screen_oracle.java writes, cut down to one stack each.
#include "ov/render/asset_source.hpp"
#include "ov/render/creative_items.hpp"
#include "ov/render/language.hpp"
#include "ov/render/text_component.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace ov;
using namespace ov::render;

namespace {

Language make_language() {
    MemoryAssetSource source;
    // A delimiter, because "%s (%s)" contains the `)"` that ends a plain raw
    // string.
    source.add("assets/minecraft/lang/en_us.json",
               R"JSON({"item.minecraft.potion.effect.night_vision":"Potion of Night Vision",
                   "effect.minecraft.night_vision":"Night Vision",
                   "potion.withDuration":"%s (%s)",
                   "potion.whenDrunk":"When Applied:",
                   "inventory.hotbarInfo":"Save hotbar with %1$s+%2$s",
                   "itemGroup.foodAndDrink":"Food & Drinks",
                   "block.minecraft.stone":"Stone"})JSON");
    auto language = Language::load(source, "en_us");
    REQUIRE(language.has_value());
    return std::move(*language);
}

}  // namespace

TEST_CASE("a plain component is its text, reset to white", "[component]") {
    const Language language = make_language();
    CHECK(flatten_component(R"({"text":"Stone"})", language) == "\xC2\xA7rStone");
    CHECK(flatten_component(R"("Stone")", language) == "\xC2\xA7rStone");
    CHECK(strip_formatting(flatten_component(R"({"text":"Stone"})", language)) == "Stone");
}

TEST_CASE("a translation substitutes its arguments in their own style", "[component]") {
    const Language language = make_language();
    const std::string line  = flatten_component(
        R"({"translate":"potion.withDuration","color":"blue",
            "with":[{"translate":"effect.minecraft.night_vision"},"03:00"]})",
        language);
    CHECK(strip_formatting(line) == "Night Vision (03:00)");
    // Blue throughout: the arguments inherit the parent's colour.
    CHECK(line.find("\xC2\xA7" "9") != std::string::npos);
    CHECK(line.find("\xC2\xA7r") == std::string::npos);
}

TEST_CASE("positional arguments and keybinds", "[component]") {
    const Language language = make_language();
    const std::string line  = flatten_component(
        R"({"translate":"inventory.hotbarInfo",
            "with":[{"keybind":"key.saveToolbarActivator"},{"keybind":"key.hotbar.3"}]})",
        language);
    CHECK(strip_formatting(line) == "Save hotbar with C+3");
}

TEST_CASE("an unknown key falls back, then shows itself", "[component]") {
    const Language language = make_language();
    CHECK(strip_formatting(flatten_component(
              R"({"translate":"no.such.key","fallback":"Fallback"})", language))
          == "Fallback");
    CHECK(strip_formatting(flatten_component(R"({"translate":"no.such.key"})", language))
          == "no.such.key");
}

TEST_CASE("formats and the extra list inherit", "[component]") {
    const Language language = make_language();
    const std::string line  = flatten_component(
        R"({"text":"A","italic":true,"color":"dark_purple","extra":[{"text":"B","bold":true}]})",
        language);
    // A: dark purple, italic. B: dark purple, italic *and* bold.
    CHECK(line == "\xC2\xA7" "5\xC2\xA7oA\xC2\xA7" "5\xC2\xA7l\xC2\xA7oB");
}

TEST_CASE("format_translation", "[component]") {
    const std::array<std::string, 2> args{"a", "b"};
    CHECK(format_translation("%s-%s", args) == "a-b");
    CHECK(format_translation("%2$s %1$s", args) == "b a");
    CHECK(format_translation("100%%", args) == "100%");
    CHECK(format_translation("%s %s %s", args) == "a b %s");
}

TEST_CASE("the creative item dump loads and keys by occurrence", "[creative]") {
    const Language language = make_language();
    auto items = CreativeItems::parse(R"({"version":"1.20.1","stacks":[
        {"item":"minecraft:stone","snbt":null,
         "tooltip":[{"text":"Stone"}],
         "search_tooltip":[{"text":"Stone"},{"translate":"itemGroup.foodAndDrink","color":"blue"}],
         "tints":[-1,-1,-1],"max_damage":0,"rarity":"COMMON"},
        {"item":"minecraft:potion","snbt":"{Potion:\"minecraft:water\"}",
         "tooltip":[{"text":"Water Bottle"}],"search_tooltip":[],
         "tints":[3694022,-1,-1],"max_damage":0,"rarity":"COMMON"},
        {"item":"minecraft:potion","snbt":"{Potion:\"minecraft:night_vision\"}",
         "tooltip":[],
         "search_tooltip":[{"translate":"item.minecraft.potion.effect.night_vision"},
                    {"translate":"itemGroup.foodAndDrink","color":"blue"},
                    {"translate":"potion.withDuration","color":"blue",
                     "with":[{"translate":"effect.minecraft.night_vision"},"03:00"]}],
         "tints":[2039713,-1,-1],"max_damage":0,"rarity":"COMMON"}],
      "queries":[{"query":"night","results":[
         {"item":"minecraft:potion","snbt":"{Potion:\"minecraft:night_vision\"}"}]}],
      "hotbars":[{"index":0,"stack":null,"tooltip":[]}]})",
                                      language);
    REQUIRE(items.has_value());
    CHECK(items->size() == 3);

    const CreativeItemInfo* stone = items->first("minecraft:stone");
    REQUIRE(stone != nullptr);
    REQUIRE(stone->search_tooltip.size() == 2);
    CHECK(strip_formatting(stone->search_tooltip[1]) == "Food & Drinks");

    const CreativeItemInfo* vision = items->find("minecraft:potion", 1);
    REQUIRE(vision != nullptr);
    CHECK(vision->tints[0] == 2039713);
    CHECK(vision->search_text == "potion of night vision\nnight vision (03:00)");
    CHECK(items->find("minecraft:potion", 2) == nullptr);

    REQUIRE(items->queries().size() == 1);
    REQUIRE(items->queries()[0].results.size() == 1);
    CHECK(items->queries()[0].results[0].second == 1);
    CHECK(items->hotbar_hints().size() == 1);
}
