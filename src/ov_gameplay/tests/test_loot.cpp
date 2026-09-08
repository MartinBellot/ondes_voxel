#include "ov/gameplay/loot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<LootTables>              loot;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        Loaded     out;
        auto       blocks = registry::BlockRegistry::load(path);
        auto       regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
            out.loot.emplace(*out.blocks, *out.registries);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] registry::BlockStateId state_of(std::string_view name) {
    const auto id = loaded().blocks->find_block(name);
    REQUIRE(id.has_value());
    return loaded().blocks->default_state(*id);
}

[[nodiscard]] registry::ProtocolId item_of(std::string_view name) {
    const auto items = loaded().registries->find("minecraft:item");
    REQUIRE(items.has_value());
    const auto id = loaded().registries->protocol_id(*items, name);
    REQUIRE(id.has_value());
    return *id;
}

/// Roll a table `times` and total what came out, by item.
[[nodiscard]] std::map<registry::ProtocolId, i64> roll(registry::BlockStateId state,
                                                       const Held& held, int times) {
    // A fixed state, so a failing test fails the same way twice.
    math::XoroshiroRandomSource         random{0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL};
    std::map<registry::ProtocolId, i64> totals;
    std::vector<Drop>                   out;
    for (int i = 0; i < times; ++i) {
        out.clear();
        loaded().loot->drops(state, held, random, out);
        for (const Drop& drop : out) {
            totals[drop.item] += drop.count;
        }
    }
    return totals;
}

}  // namespace

TEST_CASE("silk touch takes the block itself", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    const auto stone = state_of("minecraft:stone");

    Held bare;
    REQUIRE(roll(stone, bare, 8) ==
            std::map<registry::ProtocolId, i64>{{item_of("minecraft:cobblestone"), 8}});

    Held silk;
    silk.item       = item_of("minecraft:diamond_pickaxe");
    silk.silk_touch = 1;
    REQUIRE(roll(stone, silk, 8) ==
            std::map<registry::ProtocolId, i64>{{item_of("minecraft:stone"), 8}});
}

TEST_CASE("a block whose table lives under another name still drops", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    // wall_torch.json does not exist: the block points at blocks/torch. Deriving
    // the table from the block's own name works for 920 blocks out of 1003,
    // which is exactly enough to look right.
    REQUIRE(roll(state_of("minecraft:wall_torch"), Held{}, 4) ==
            std::map<registry::ProtocolId, i64>{{item_of("minecraft:torch"), 4}});
    REQUIRE(roll(state_of("minecraft:oak_wall_sign"), Held{}, 4) ==
            std::map<registry::ProtocolId, i64>{{item_of("minecraft:oak_sign"), 4}});

    // And a block routed to the empty table has no drops at all, which is not
    // the same as a table that rolled nothing.
    REQUIRE_FALSE(loaded().loot->has_table(state_of("minecraft:bedrock")));
    REQUIRE(roll(state_of("minecraft:bedrock"), Held{}, 4).empty());
}

TEST_CASE("fortune multiplies ore drops", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    const auto diamond_ore = state_of("minecraft:diamond_ore");
    const auto diamond     = item_of("minecraft:diamond");

    Held pick;
    pick.item = item_of("minecraft:diamond_pickaxe");
    REQUIRE(roll(diamond_ore, pick, 500).at(diamond) == 500);

    // The multiplier is `max(0, nextInt(level + 2) - 1) + 1`: at level 3 that is
    // 1, 1, 2, 3, 4 with equal chances, a mean of 2.2. Flooring the draw at one
    // instead gives 1.6 — which looks reasonable, and is a third short.
    for (const auto [level, low, high] :
         {std::tuple{1, 1.25, 1.42}, std::tuple{2, 1.65, 1.85}, std::tuple{3, 2.05, 2.35}}) {
        Held enchanted    = pick;
        enchanted.fortune = static_cast<u8>(level);
        const auto mean = static_cast<f64>(roll(diamond_ore, enchanted, 4000).at(diamond)) / 4000.0;
        REQUIRE(mean > low);
        REQUIRE(mean < high);
    }
}

TEST_CASE("fortune's chance table is read by level", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    const auto gravel = state_of("minecraft:gravel");
    const auto flint  = item_of("minecraft:flint");

    Held pick;
    pick.item        = item_of("minecraft:diamond_pickaxe");
    const auto plain = roll(gravel, pick, 4000);
    REQUIRE(plain.at(flint) > 300);  // one in ten
    REQUIRE(plain.at(flint) < 500);

    // The last chance in gravel's table is 1.0, so Fortune III turns every
    // block into flint and no gravel comes back at all.
    pick.fortune     = 3;
    const auto lucky = roll(gravel, pick, 200);
    REQUIRE(lucky.at(flint) == 200);
    REQUIRE_FALSE(lucky.contains(item_of("minecraft:gravel")));
}

TEST_CASE("a state property decides which entry wins", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    const auto wheat_block = loaded().blocks->find_block("minecraft:wheat");
    REQUIRE(wheat_block.has_value());
    const auto age = loaded().blocks->find_property(*wheat_block, "age");
    REQUIRE(age.has_value());

    const auto unripe =
        loaded().blocks->with_property(loaded().blocks->first_state(*wheat_block), *age, 0);
    const auto ripe =
        loaded().blocks->with_property(loaded().blocks->first_state(*wheat_block), *age, 7);

    // Unripe wheat gives back a seed and nothing else; ripe wheat gives the
    // crop, plus seeds from a second pool that only opens at age seven.
    const auto young = roll(unripe, Held{}, 200);
    REQUIRE(young.at(item_of("minecraft:wheat_seeds")) == 200);
    REQUIRE_FALSE(young.contains(item_of("minecraft:wheat")));

    const auto grown = roll(ripe, Held{}, 200);
    REQUIRE(grown.at(item_of("minecraft:wheat")) == 200);
    REQUIRE(grown.at(item_of("minecraft:wheat_seeds")) > 0);
}

TEST_CASE("the tool itself can be the condition", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    const auto grass = state_of("minecraft:grass");

    Held shears;
    shears.item = item_of("minecraft:shears");
    REQUIRE(roll(grass, shears, 100).at(item_of("minecraft:grass")) == 100);

    // Without them it is a one-in-eight chance of a seed and nothing otherwise.
    const auto bare = roll(grass, Held{}, 4000);
    REQUIRE(bare.at(item_of("minecraft:wheat_seeds")) > 400);
    REQUIRE(bare.at(item_of("minecraft:wheat_seeds")) < 600);
}

TEST_CASE("a uniform count is uniform", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    // Melon drops three to seven slices, a mean of five.
    const auto mean =
        static_cast<f64>(
            roll(state_of("minecraft:melon"), Held{}, 4000).at(item_of("minecraft:melon_slice"))) /
        4000.0;
    REQUIRE(mean > 4.85);
    REQUIRE(mean < 5.15);
}

TEST_CASE("a two-block plant asks about its other half", "[gameplay][loot]") {
    if (!loaded().loot) {
        SKIP("no registry pack");
    }
    const auto grass_block = loaded().blocks->find_block("minecraft:tall_grass");
    REQUIRE(grass_block.has_value());
    const auto half = loaded().blocks->find_property(*grass_block, "half");
    REQUIRE(half.has_value());

    const auto values = half->values;
    const auto upper_index =
        static_cast<u16>(std::distance(values.begin(), std::ranges::find(values, "upper")));
    const auto lower_index =
        static_cast<u16>(std::distance(values.begin(), std::ranges::find(values, "lower")));
    const auto base  = loaded().blocks->first_state(*grass_block);
    const auto lower = loaded().blocks->with_property(base, *half, lower_index);
    const auto upper = loaded().blocks->with_property(base, *half, upper_index);

    math::XoroshiroRandomSource random{0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL};
    std::vector<Drop>           out;

    // Whole plant: one seed in eight. The real server gave 22 over 200 rolls.
    i64 seeds = 0;
    for (int i = 0; i < 4000; ++i) {
        out.clear();
        loaded().loot->drops(lower, Held{}, random, out, Neighbours{.above = upper});
        for (const Drop& drop : out) {
            seeds += drop.count;
        }
    }
    REQUIRE(seeds > 400);
    REQUIRE(seeds < 600);

    // Top already gone: nothing at all, which is also what the server answers.
    out.clear();
    for (int i = 0; i < 400; ++i) {
        loaded().loot->drops(lower, Held{}, random, out, Neighbours{});
    }
    REQUIRE(out.empty());
}
