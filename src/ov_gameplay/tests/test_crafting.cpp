#include "ov/gameplay/crafting.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
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

/// A grid written the way a player sees it: one string per row, one character
/// per cell, and a legend. `.` is an empty cell.
[[nodiscard]] CraftingGrid grid_of(std::initializer_list<const char*>        rows,
                                   const std::map<char, std::string_view>&   legend,
                                   u8 width = 3, u8 height = 3) {
    CraftingGrid grid;
    grid.width  = width;
    grid.height = height;
    usize y     = 0;
    for (const char* row : rows) {
        for (usize x = 0; x < width; ++x) {
            const char symbol = row[x];
            if (symbol == '.') {
                continue;
            }
            const auto found = legend.find(symbol);
            REQUIRE(found != legend.end());
            grid.at(x, y) = RecipeStack{item_of(found->second), 1};
        }
        ++y;
    }
    return grid;
}

}  // namespace

TEST_CASE("the pack carries every recipe the datapack has", "[crafting]") {
    REQUIRE(loaded().book.has_value());
    const RecipeBook& book = *loaded().book;

    // 1174 files in 1.20.1's recipe folder. All of them are in the pack: the
    // fourteen special ones are declarations rather than data, but they are
    // there, because the client's recipe book asks for them by name.
    CHECK(book.size() == 1174);

    std::map<registry::RecipeKind, int> per_kind;
    for (RecipeIndex index = 0; index < book.size(); ++index) {
        per_kind[book.kind(index)] += 1;
    }
    CHECK(per_kind[registry::RecipeKind::CraftingShaped] == 590);
    CHECK(per_kind[registry::RecipeKind::CraftingShapeless] == 232);
    CHECK(per_kind[registry::RecipeKind::Stonecutting] == 201);
    CHECK(per_kind[registry::RecipeKind::Smelting] == 70);
    CHECK(per_kind[registry::RecipeKind::Blasting] == 24);
    CHECK(per_kind[registry::RecipeKind::Smoking] == 9);
    CHECK(per_kind[registry::RecipeKind::CampfireCooking] == 9);
    CHECK(per_kind[registry::RecipeKind::SmithingTransform] == 9);
    CHECK(per_kind[registry::RecipeKind::SmithingTrim] == 16);
    // Thirteen `crafting_special_*` plus `crafting_decorated_pot`, which has no
    // data either. Named here so that a version bump which adds one fails
    // loudly rather than quietly not crafting.
    CHECK(per_kind[registry::RecipeKind::Special] == 14);
}

TEST_CASE("a shaped recipe matches anywhere it fits", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // The wooden pickaxe: three planks over two sticks, in the top-left corner
    // of a 3x3.
    const std::map<char, std::string_view> legend{{'P', "minecraft:oak_planks"},
                                                  {'S', "minecraft:stick"}};
    const auto                             pickaxe = item_of("minecraft:wooden_pickaxe");

    const auto top = match_crafting(book, grid_of({"PPP", ".S.", ".S."}, legend));
    REQUIRE(top.has_value());
    CHECK(top->result.item == pickaxe);
    CHECK(top->result.count == 1);

    // A two-wide recipe placed in each of the four corners it fits.
    const auto planks = item_of("minecraft:oak_planks");
    for (usize dx = 0; dx < 2; ++dx) {
        for (usize dy = 0; dy < 2; ++dy) {
            CraftingGrid grid;
            grid.at(dx, dy)         = RecipeStack{planks, 1};
            grid.at(dx + 1, dy)     = RecipeStack{planks, 1};
            grid.at(dx, dy + 1)     = RecipeStack{planks, 1};
            grid.at(dx + 1, dy + 1) = RecipeStack{planks, 1};
            const auto made         = match_crafting(book, grid);
            REQUIRE(made.has_value());
            CHECK(item_name(made->result.item) == "minecraft:crafting_table");
        }
    }
}

TEST_CASE("a shaped recipe matches its mirror", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // The bow is the classic asymmetric recipe: three strings on the right,
    // sticks curving away. Its mirror must craft a bow too — vanilla mirrors,
    // and a player who learned the shape the other way round is not wrong.
    const std::map<char, std::string_view> legend{{'S', "minecraft:stick"},
                                                  {'T', "minecraft:string"}};
    const auto direct  = match_crafting(book, grid_of({".ST", "S.T", ".ST"}, legend));
    const auto mirror  = match_crafting(book, grid_of({"TS.", "T.S", "TS."}, legend));
    REQUIRE(direct.has_value());
    REQUIRE(mirror.has_value());
    CHECK(item_name(direct->result.item) == "minecraft:bow");
    CHECK(direct->result.item == mirror->result.item);
}

TEST_CASE("a shapeless recipe ignores order", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // Mushroom stew: a bowl and two mushrooms, listed in one order and made in
    // any. A matcher that compared sorted lists would pass this and still fail
    // the recipes whose ingredients overlap, which the parity table covers.
    const std::map<char, std::string_view> legend{{'R', "minecraft:red_mushroom"},
                                                  {'B', "minecraft:brown_mushroom"},
                                                  {'O', "minecraft:bowl"}};
    const auto one = match_crafting(book, grid_of({"RBO", "...", "..."}, legend));
    const auto two = match_crafting(book, grid_of({"..O", ".B.", "R.."}, legend));
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(item_name(one->result.item) == "minecraft:mushroom_stew");
    CHECK(one->result.item == two->result.item);
}

TEST_CASE("a tag ingredient accepts every member", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // The pickaxe's head is `#minecraft:planks`, so every plank makes one.
    for (const auto* wood : {"minecraft:oak_planks", "minecraft:spruce_planks",
                             "minecraft:birch_planks", "minecraft:crimson_planks",
                             "minecraft:bamboo_planks"}) {
        const std::map<char, std::string_view> legend{{'P', wood},
                                                      {'S', "minecraft:stick"}};
        const auto made = match_crafting(book, grid_of({"PPP", ".S.", ".S."}, legend));
        REQUIRE(made.has_value());
        CHECK(item_name(made->result.item) == "minecraft:wooden_pickaxe");
    }
}

TEST_CASE("a pattern's blank must stay blank", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // A stick dropped into the pickaxe's empty corner is not a pickaxe. This is
    // the failure a matcher that only checks the filled cells would let
    // through, and it would make almost every grid craft something.
    const std::map<char, std::string_view> legend{{'P', "minecraft:oak_planks"},
                                                  {'S', "minecraft:stick"}};
    CHECK_FALSE(match_crafting(book, grid_of({"PPP", "SS.", ".S."}, legend)).has_value());
    CHECK_FALSE(match_crafting(book, {}).has_value());
}

TEST_CASE("the spyglass survives its untrimmed pattern", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // Written as `[" # ", " X ", " X "]` in the datapack: three cells wide for
    // one useful column. The game trims it at load, and so does the emitter.
    // Without that, this is the one item in 1.20.1 that can never be made.
    const std::map<char, std::string_view> legend{{'A', "minecraft:amethyst_shard"},
                                                  {'C', "minecraft:copper_ingot"}};
    const auto made = match_crafting(book, grid_of({"A..", "C..", "C.."}, legend));
    REQUIRE(made.has_value());
    CHECK(item_name(made->result.item) == "minecraft:spyglass");
}

TEST_CASE("crafting consumes one of each and returns the bucket", "[crafting]") {
    const RecipeBook& book = *loaded().book;

    // A cake takes three milk buckets, two sugar, one egg and three wheat, and
    // gives the three buckets back. That return is Java code in vanilla, and
    // it was measured rather than assumed.
    const std::map<char, std::string_view> legend{{'M', "minecraft:milk_bucket"},
                                                  {'S', "minecraft:sugar"},
                                                  {'E', "minecraft:egg"},
                                                  {'W', "minecraft:wheat"}};
    CraftingGrid grid = grid_of({"MMM", "SES", "WWW"}, legend);

    const auto made = match_crafting(book, grid);
    REQUIRE(made.has_value());
    CHECK(item_name(made->result.item) == "minecraft:cake");

    const CraftConsumption after = consume_craft(book, grid, *made);
    CHECK(after.overflow_count == 0);
    int buckets = 0;
    for (usize cell = 0; cell < after.grid.cell_count(); ++cell) {
        if (after.grid.cells[cell].empty()) {
            continue;
        }
        CHECK(item_name(after.grid.cells[cell].item) == "minecraft:bucket");
        ++buckets;
    }
    CHECK(buckets == 3);
}

TEST_CASE("a shift-click crafts until the grid runs out", "[crafting]") {
    const RecipeBook& book = *loaded().book;
    const auto        planks = item_of("minecraft:oak_planks");

    // Four planks make one crafting table. With seven in each cell, seven
    // tables come out and nothing is left — this is the loop a shift-click on
    // the result slot runs, and the case that turns into an infinite one when
    // the count is taken from the wrong cell.
    CraftingGrid grid;
    grid.at(0, 0) = grid.at(1, 0) = RecipeStack{planks, 7};
    grid.at(0, 1) = grid.at(1, 1) = RecipeStack{planks, 5};

    const auto made = match_crafting(book, grid);
    REQUIRE(made.has_value());
    CHECK(max_crafts(book, grid, *made) == 5);

    int  crafted = 0;
    auto current = grid;
    while (const auto again = match_crafting(book, current)) {
        current = consume_craft(book, current, *again).grid;
        ++crafted;
        REQUIRE(crafted < 64);
    }
    CHECK(crafted == 5);
    // Two planks each are left in the top row, and the recipe no longer fits.
    CHECK(current.at(0, 0).count == 2);
    CHECK(current.at(0, 1).empty());
}

TEST_CASE("a 2x2 grid can only make what fits in it", "[crafting]") {
    const RecipeBook& book = *loaded().book;
    const auto        planks = item_of("minecraft:oak_planks");
    const auto        stick  = item_of("minecraft:stick");

    CraftingGrid two;
    two.width = two.height = 2;
    two.cells[0]           = RecipeStack{planks, 1};
    two.cells[1]           = RecipeStack{planks, 1};
    two.cells[2]           = RecipeStack{planks, 1};
    two.cells[3]           = RecipeStack{planks, 1};
    const auto table       = match_crafting(book, two);
    REQUIRE(table.has_value());
    CHECK(item_name(table->result.item) == "minecraft:crafting_table");

    // Sticks: two planks stacked, which fits a 2x2 as well.
    CraftingGrid sticks;
    sticks.width = sticks.height = 2;
    sticks.cells[0]              = RecipeStack{planks, 1};
    sticks.cells[2]              = RecipeStack{planks, 1};
    const auto made              = match_crafting(book, sticks);
    REQUIRE(made.has_value());
    CHECK(made->result.item == stick);
    CHECK(made->result.count == 4);
}
