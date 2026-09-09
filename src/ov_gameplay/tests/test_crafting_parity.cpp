// Every grid a real 1.20.1 server was asked about, replayed against ours.
//
// The table is written by scripts/measure_crafting.py, which fills a crafting
// table through a probe client and reads back what the game puts in the result
// slot — every shaped recipe in every position it fits and in its mirror, every
// shapeless recipe with its ingredients shuffled, and a batch of grids that
// must produce nothing at all. That last group is the half that catches a
// matcher which is too generous, because everything that should work will.
//
// The table is regenerated locally and never committed: it is derived from
// Mojang's own data. When it is missing the test says so and skips, rather than
// passing on an empty comparison.
#include "ov/gameplay/crafting.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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

}  // namespace

TEST_CASE("every grid the real server was asked about", "[crafting][parity]") {
    const auto table = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                       "normalized" / "crafting_grids.txt";
    if (!std::filesystem::exists(table)) {
        WARN("no crafting_grids.txt — run scripts/measure_crafting.py then "
             "scripts/check_crafting.py");
        return;
    }
    REQUIRE(loaded().book.has_value());
    const RecipeBook& book = *loaded().book;

    const auto items = loaded().registries->find("minecraft:item");
    REQUIRE(items.has_value());
    // One pass over the item registry rather than a linear search per cell:
    // there are tens of thousands of cells in the table.
    std::unordered_map<std::string, registry::ProtocolId> id_of;
    for (const std::string_view name : loaded().registries->entries(*items)) {
        const auto id = loaded().registries->protocol_id(*items, name);
        if (id) {
            id_of.emplace(std::string{name}, *id);
        }
    }

    std::ifstream input{table};
    REQUIRE(input.is_open());

    usize                    checked = 0;
    usize                    agreed  = 0;
    std::vector<std::string> disagreements;
    std::string              line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields{line};
        std::string        expected_item;
        int                expected_count = 0;
        fields >> expected_item >> expected_count;

        CraftingGrid grid;
        grid.width = grid.height = 3;
        bool usable              = true;
        for (usize cell = 0; cell < 9; ++cell) {
            std::string name;
            fields >> name;
            if (name == "-") {
                continue;
            }
            const auto found = id_of.find(name);
            if (found == id_of.end()) {
                usable = false;
                break;
            }
            grid.cells[cell] = RecipeStack{found->second, 1};
        }
        if (!usable) {
            continue;
        }

        ++checked;
        const auto made = match_crafting(book, grid);
        const bool same =
            expected_item == "-"
                ? !made.has_value()
                : made.has_value() && id_of.count(expected_item) != 0 &&
                      made->result.item == id_of.at(expected_item) &&
                      made->result.count == expected_count;
        if (same) {
            ++agreed;
        } else if (disagreements.size() < 20) {
            std::string got = "nothing";
            if (made) {
                got = std::to_string(made->result.item) + " x" +
                      std::to_string(made->result.count);
            }
            disagreements.push_back(line + "  -> got " + got);
        }
    }

    INFO("grids compared: " << checked << ", identical: " << agreed);
    for (const std::string& one : disagreements) {
        UNSCOPED_INFO(one);
    }
    CHECK(checked > 3000);
    CHECK(agreed == checked);
}
