// Trees and vegetation: the parts whose *number of draws* is the whole answer.
//
// A tree is the longest run of random draws in worldgen — height, foliage
// height, foliage radius, then a draw per corner of every leaf row, then one
// per side of every log a decorator touches. None of that is visible in a
// screenshot, and a single draw out of place moves every tree after it in the
// chunk. So these tests pin the things that cannot be checked by looking:
//
//   * the order and the count of the draws a tree makes;
//   * the iteration order of a `java.util.HashSet<BlockPos>`, which is what the
//     decorators walk and therefore part of the seed;
//   * that a survival rule is *refused* rather than guessed;
//   * that the datapack really loads the placers it is supposed to.

#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/tree_feature.hpp"
#include "ov/worldgen/vegetation_feature.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path source_root() {
    return std::filesystem::path{OV_SOURCE_DIR};
}

[[nodiscard]] std::filesystem::path data_root() {
    return source_root() / "data" / "vanilla" / "1.20.1" / "generated" / "data" / "minecraft";
}

[[nodiscard]] std::filesystem::path pack_path() {
    return source_root() / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

/// Grass on top of dirt on top of stone, and air above. Enough ground for a
/// tree to decide it may grow.
class GroundLevel final : public FeatureLevel {
public:
    GroundLevel(registry::BlockStateId grass, registry::BlockStateId dirt,
                registry::BlockStateId stone, i32 surface)
        : grass_(grass), dirt_(dirt), stone_(stone), surface_(surface) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        const auto found = written_.find(key(x, y, z));
        if (found != written_.end()) {
            return found->second;
        }
        if (y > surface_) {
            return registry::kAirState;
        }
        if (y == surface_) {
            return grass_;
        }
        return y > surface_ - 4 ? dirt_ : stone_;
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        if (outside_build_height(y)) {
            return false;
        }
        written_[key(x, y, z)] = state;
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override {
        return surface_ + 1;
    }

    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:plains";
    }

    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    [[nodiscard]] const std::map<i64, registry::BlockStateId>& written() const {
        return written_;
    }

private:
    [[nodiscard]] static i64 key(i32 x, i32 y, i32 z) {
        return (static_cast<i64>(x) << 40) | ((static_cast<i64>(z) & 0xFFFFF) << 20) |
               static_cast<i64>(y + 64);
    }

    registry::BlockStateId                grass_;
    registry::BlockStateId                dirt_;
    registry::BlockStateId                stone_;
    i32                                   surface_;
    std::map<i64, registry::BlockStateId> written_;
};

/// The z of a `GroundLevel` key, sign-extended out of its twenty bits.
[[nodiscard]] i32 sign_extend_z(i64 key) {
    const auto raw = static_cast<u32>((key >> 20) & 0xFFFFF);
    return static_cast<i32>(raw << 12) >> 12;
}

/// Nothing lists anything, which is what a nested placed feature sees.
class NoBiomeFeatures final : public BiomeFeatures {
public:
    [[nodiscard]] bool lists(std::string_view, std::string_view) const override { return true; }
};

}  // namespace

TEST_CASE("a java hash set of positions iterates in the game's order", "[worldgen][tree]") {
    // `Vec3i.hashCode()` is `(y + z * 31) * 31 + x`, and the map spreads it
    // before masking. Both halves are pinned here because the decorators walk
    // this order and draw once per element.
    CHECK(java_block_pos_hash({0, 0, 0}) == 0);
    CHECK(java_block_pos_hash({1, 0, 0}) == 1);
    CHECK(java_block_pos_hash({0, 1, 0}) == 31);
    CHECK(java_block_pos_hash({0, 0, 1}) == 31 * 31);
    CHECK(java_block_pos_hash({-1, -1, -1}) == (-1 + -1 * 31) * 31 + -1);

    SECTION("insertion order is not iteration order") {
        // Sixteen slots and a load factor of three quarters: twelve entries fit
        // without a resize, and the walk is by bucket rather than by arrival.
        std::vector<BlockPos> inserted;
        for (i32 i = 0; i < 12; ++i) {
            inserted.push_back({i, 0, 0});
        }
        const auto order = java_hash_order(inserted);
        REQUIRE(order.size() == inserted.size());
        // x from 0 to 11 hashes to 0..11, so the buckets are in x order and the
        // set happens to come back sorted. That is the point: it is *bucket*
        // order, and it is reproducible.
        for (usize i = 0; i < order.size(); ++i) {
            CHECK(order[i].x == static_cast<i32>(i));
        }
    }

    SECTION("a repeat is stored once") {
        const std::vector<BlockPos> inserted{{1, 2, 3}, {1, 2, 3}, {4, 5, 6}};
        CHECK(java_hash_order(inserted).size() == 2);
    }

    SECTION("resizing keeps every element") {
        std::vector<BlockPos> inserted;
        for (i32 y = 0; y < 12; ++y) {
            for (i32 x = 0; x < 12; ++x) {
                inserted.push_back({x, y, 0});
            }
        }
        const auto        order = java_hash_order(inserted);
        std::set<i64>     seen;
        for (const BlockPos& pos : order) {
            seen.insert((static_cast<i64>(pos.x) << 20) | pos.y);
        }
        CHECK(order.size() == inserted.size());
        CHECK(seen.size() == inserted.size());
    }
}

TEST_CASE("a survival rule is named or refused, never guessed", "[worldgen][tree]") {
    CHECK(plant_survival_rule("minecraft:oak_sapling") == PlantSurvivalRule::DirtOrFarmland);
    CHECK(plant_survival_rule("minecraft:dead_bush") == PlantSurvivalRule::DeadBush);
    CHECK(plant_survival_rule("minecraft:lily_pad") == PlantSurvivalRule::Waterlily);
    CHECK(plant_survival_rule("minecraft:azalea") == PlantSurvivalRule::DirtOrClay);
    // The two mushrooms read the light level, which worldgen does not compute:
    // absent on purpose. Fire has a rule since the fire wave — its sturdy-floor
    // half; the flammable-neighbour half is refused inside the rule.
    CHECK(!plant_survival_rule("minecraft:brown_mushroom").has_value());
    // Measured with it: 43/43 fire and 6/6 soul fire blocks where the game put
    // them in 25 full Nether chunks.
    CHECK(plant_survival_rule("minecraft:fire") == PlantSurvivalRule::Fire);
    CHECK(plant_survival_rule("minecraft:soul_fire") == PlantSurvivalRule::SoulFire);
    CHECK(!plant_survival_rule("minecraft:stone").has_value());

    CHECK(is_double_plant("minecraft:tall_grass"));
    CHECK(is_double_plant("minecraft:sunflower"));
    CHECK(!is_double_plant("minecraft:poppy"));
}

TEST_CASE("the survival table is sorted, because the lookup is a binary search",
          "[worldgen][tree]") {
    // A table out of order silently answers "no rule" for the entries after the
    // first inversion, and every feature that places one of them stops loading.
    // That happened: `birch_sapling` filed after `blue_orchid` cost every tree
    // selector in the game.
    static constexpr std::array<std::string_view, 8> kProbe{
        "minecraft:acacia_sapling", "minecraft:birch_sapling", "minecraft:blue_orchid",
        "minecraft:cherry_sapling", "minecraft:oak_sapling",   "minecraft:spruce_sapling",
        "minecraft:tall_grass",     "minecraft:white_tulip",
    };
    for (const std::string_view name : kProbe) {
        INFO(name);
        CHECK(plant_survival_rule(name).has_value());
    }
}

TEST_CASE("the datapack's trees and vegetation load", "[worldgen][tree]") {
    if (!std::filesystem::exists(pack_path()) || !std::filesystem::is_directory(data_root())) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    // One placed feature per trunk placer and per foliage placer. A placer
    // quietly dropped shows up here as a name that stopped loading, which is
    // the only way an interpreter can be tested for completeness.
    static constexpr std::array<std::string_view, 13> kTrees{
        "minecraft:oak_checked",                // straight trunk, blob foliage
        "minecraft:fancy_oak_checked",          // fancy trunk, fancy foliage
        "minecraft:dark_oak_checked",           // dark oak trunk, dark oak foliage
        "minecraft:spruce_checked",             // spruce foliage
        "minecraft:pine_checked",               // pine foliage
        "minecraft:acacia_checked",             // forking trunk, acacia foliage
        "minecraft:jungle_bush",                // bush foliage
        "minecraft:mega_spruce_checked",        // giant trunk, mega pine, alter_ground
        "minecraft:mega_pine_checked",          // giant trunk, mega pine
        "minecraft:mega_jungle_tree_checked",   // mega jungle trunk, jungle foliage
        "minecraft:mangrove_checked",           // upwards branching trunk, root placer
        "minecraft:tall_mangrove_checked",      // the same with a taller trunk
        "minecraft:cherry_checked",             // cherry trunk and foliage
    };
    for (const std::string_view name : kTrees) {
        INFO(name);
        CHECK(registry->placed(name) != nullptr);
    }

    // The count is the headline number of this work and is checked, not
    // described: 39 of 194 before the trees and the vegetation, 113 after.
    CHECK(registry->configured_count() >= 113);
    CHECK(registry->placed_count() >= 134);
}

TEST_CASE("a straight trunk with blob foliage draws in the game's order", "[worldgen][tree]") {
    if (!std::filesystem::exists(pack_path()) || !std::filesystem::is_directory(data_root())) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    const auto* birch = registry->placed("minecraft:birch_checked");
    REQUIRE(birch != nullptr);

    const auto grass = pack->find_block("minecraft:grass_block");
    const auto dirt  = pack->find_block("minecraft:dirt");
    const auto stone = pack->find_block("minecraft:stone");
    REQUIRE(grass.has_value());
    REQUIRE(dirt.has_value());
    REQUIRE(stone.has_value());

    GroundLevel     level{pack->default_state(*grass), pack->default_state(*dirt),
                      pack->default_state(*stone), 64};
    NoBiomeFeatures biomes;
    FeatureContext  context;
    context.blocks       = &*pack;
    context.feature_name = birch->name;
    context.biomes       = &biomes;

    FeatureRandom random{FeatureRandom::Kind::Xoroshiro, 12345};
    CHECK(birch->feature->place(context, level, random, {0, 65, 0}));

    // A birch is `base_height 5 + nextInt(3) + nextInt(1)` tall, so between
    // five and seven logs, with the trunk in one column.
    i32 logs = 0;
    for (i32 y = 65; y < 80; ++y) {
        if (pack->block_name(pack->block_of(level.block_at(0, y, 0))) == "minecraft:birch_log") {
            ++logs;
        }
    }
    CHECK(logs >= 5);
    CHECK(logs <= 7);

    // And leaves above it, of the right species — the foliage provider is not
    // shared with the trunk's.
    i32 leaves = 0;
    for (i32 y = 65; y < 82; ++y) {
        for (i32 x = -3; x <= 3; ++x) {
            for (i32 z = -3; z <= 3; ++z) {
                if (pack->block_name(pack->block_of(level.block_at(x, y, z))) ==
                    "minecraft:birch_leaves") {
                    ++leaves;
                }
            }
        }
    }
    CHECK(leaves > 20);

    // Determinism: the same seed twice gives the same tree, block for block.
    GroundLevel   again{pack->default_state(*grass), pack->default_state(*dirt),
                      pack->default_state(*stone), 64};
    FeatureRandom repeat{FeatureRandom::Kind::Xoroshiro, 12345};
    CHECK(birch->feature->place(context, again, repeat, {0, 65, 0}));
    CHECK(again.written() == level.written());
}

TEST_CASE("a tree refuses to grow where nothing holds it", "[worldgen][tree]") {
    if (!std::filesystem::exists(pack_path()) || !std::filesystem::is_directory(data_root())) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    const auto* birch = registry->placed("minecraft:birch_checked");
    REQUIRE(birch != nullptr);

    const auto stone = pack->find_block("minecraft:stone");
    REQUIRE(stone.has_value());
    // Stone all the way up, so `would_survive` says no and the pipeline throws
    // the position away before the tree is ever built.
    GroundLevel     level{pack->default_state(*stone), pack->default_state(*stone),
                      pack->default_state(*stone), 64};
    NoBiomeFeatures biomes;
    FeatureContext  context;
    context.blocks       = &*pack;
    context.feature_name = birch->name;
    context.biomes       = &biomes;

    FeatureRandom random{FeatureRandom::Kind::Xoroshiro, 12345};
    bool          wrote = false;
    expand(birch->placement, context, level, random, {0, 65, 0}, [&](BlockPos at) {
        wrote |= birch->feature->place(context, level, random, at);
    });
    CHECK(!wrote);
    CHECK(level.written().empty());
}

TEST_CASE("a random patch always spends its tries", "[worldgen][tree]") {
    if (!std::filesystem::exists(pack_path()) || !std::filesystem::is_directory(data_root())) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    const auto* grass_patch = registry->placed("minecraft:patch_grass_plain");
    if (grass_patch == nullptr) {
        SUCCEED("patch_grass_plain is not built");
        return;
    }

    const auto stone = pack->find_block("minecraft:stone");
    REQUIRE(stone.has_value());
    GroundLevel     barren{pack->default_state(*stone), pack->default_state(*stone),
                       pack->default_state(*stone), 64};
    NoBiomeFeatures biomes;
    FeatureContext  context;
    context.blocks       = &*pack;
    context.feature_name = grass_patch->name;
    context.biomes       = &biomes;

    // The number of draws does not depend on the ground. This is the property
    // that lets the survival table be a table: a wrong rule costs blocks and
    // never costs the seed. Two runs from the same seed over two different
    // worlds must leave the generator in the same state.
    FeatureRandom over_stone{FeatureRandom::Kind::Xoroshiro, 999};
    (void)grass_patch->feature->place(context, barren, over_stone, {0, 65, 0});

    const auto  grass = pack->find_block("minecraft:grass_block");
    const auto  dirt  = pack->find_block("minecraft:dirt");
    REQUIRE(grass.has_value());
    REQUIRE(dirt.has_value());
    GroundLevel   fertile{pack->default_state(*grass), pack->default_state(*dirt),
                        pack->default_state(*stone), 64};
    FeatureRandom over_grass{FeatureRandom::Kind::Xoroshiro, 999};
    (void)grass_patch->feature->place(context, fertile, over_grass, {0, 65, 0});

    const i32 after_stone = over_stone.next_int();
    const i32 after_grass = over_grass.next_int();
    CHECK(after_stone == after_grass);
}

TEST_CASE("a fancy oak's canopy is not an inverted cone", "[worldgen][tree]") {
    // The rule this guards was read off a probe world (scripts/probe_tree.sh)
    // rather than reasoned about: the rows run from `offset` down to
    // `offset - height`, the first and last are one narrower than the radius,
    // and a cell is written when `x² + z² < range² + range`. The reading that
    // stood here before made a cone widest at its bottom row, and no fancy oak
    // in either reference world ever came out right.
    if (!std::filesystem::exists(pack_path()) || !std::filesystem::is_directory(data_root())) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    const auto* fancy = registry->configured("minecraft:fancy_oak");
    REQUIRE(fancy != nullptr);

    const auto grass = pack->find_block("minecraft:grass_block");
    const auto dirt  = pack->find_block("minecraft:dirt");
    const auto stone = pack->find_block("minecraft:stone");
    REQUIRE(grass.has_value());
    REQUIRE(dirt.has_value());
    REQUIRE(stone.has_value());

    GroundLevel     level{pack->default_state(*grass), pack->default_state(*dirt),
                      pack->default_state(*stone), 64};
    NoBiomeFeatures biomes;
    FeatureContext  context;
    context.blocks       = &*pack;
    context.feature_name = "minecraft:fancy_oak";
    context.biomes       = &biomes;

    FeatureRandom random{FeatureRandom::Kind::Xoroshiro, 987};
    CHECK(fancy->place(context, level, random, {0, 65, 0}));

    // The lowest row of a canopy must never be its widest. With `radius` 2 and
    // an attachment on the trunk column, the old reading gave the bottom row a
    // half-width of three and the top row none at all.
    std::map<i32, i32> half_width;
    std::map<i32, i32> per_row;
    for (const auto& [where, state] : level.written()) {
        if (pack->block_name(pack->block_of(state)) != "minecraft:oak_leaves") {
            continue;
        }
        const i32 x = static_cast<i32>(where >> 40);
        const i32 z = sign_extend_z(where);
        const i32 y = static_cast<i32>(where & 0xFFFFF) - 64;
        half_width[y] = std::max(half_width[y], std::max(std::abs(x), std::abs(z)));
        per_row[y] += 1;
    }
    REQUIRE(half_width.size() >= 5);
    const i32 lowest  = half_width.begin()->first;
    const i32 highest = half_width.rbegin()->first;
    CHECK(half_width[lowest] < half_width[highest] + 4);
    // A range-two row of this placer holds twenty-one cells, not the thirteen
    // of a disc of radius two nor the twenty-five of a full square.
    bool saw_twenty_one = false;
    for (const auto& [y, count] : per_row) {
        (void)y;
        if (count == 21 || count == 20) {
            saw_twenty_one = true;
        }
    }
    CHECK(saw_twenty_one);

    // Determinism, and the draw count with it: the same seed twice, block for
    // block, and the generator left in the same state.
    GroundLevel   again{pack->default_state(*grass), pack->default_state(*dirt),
                      pack->default_state(*stone), 64};
    FeatureRandom repeat{FeatureRandom::Kind::Xoroshiro, 987};
    CHECK(fancy->place(context, again, repeat, {0, 65, 0}));
    CHECK(again.written() == level.written());
    CHECK(repeat.next_int() == random.next_int());
}

TEST_CASE("a jungle bush draws for its corners", "[worldgen][tree]") {
    // The bush's corner is decided by a `nextInt(2)`, and the draw happens even
    // for the degenerate corner of a range-zero row — the single block at the
    // top of the bush, which is therefore there about half the time. That draw
    // was missing altogether, so every feature placed after a bush in the same
    // chunk read the generator one step early, which is why `trees_jungle`
    // scored one tree in eighteen while its trunks were in the right place.
    if (!std::filesystem::exists(pack_path()) || !std::filesystem::is_directory(data_root())) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    const auto* bush = registry->configured("minecraft:jungle_bush");
    REQUIRE(bush != nullptr);

    const auto grass = pack->find_block("minecraft:grass_block");
    const auto dirt  = pack->find_block("minecraft:dirt");
    const auto stone = pack->find_block("minecraft:stone");
    REQUIRE(grass.has_value());
    REQUIRE(dirt.has_value());
    REQUIRE(stone.has_value());

    NoBiomeFeatures biomes;
    FeatureContext  context;
    context.blocks       = &*pack;
    context.feature_name = "minecraft:jungle_bush";
    context.biomes       = &biomes;

    // Over a run of seeds, the top block is sometimes there and sometimes not,
    // and the number of leaves is not constant. Both are only possible if the
    // corner is drawn for.
    std::set<i32> leaf_counts;
    i32           tops = 0;
    constexpr i32 kSeeds = 24;
    for (i32 seed = 0; seed < kSeeds; ++seed) {
        GroundLevel   level{pack->default_state(*grass), pack->default_state(*dirt),
                          pack->default_state(*stone), 64};
        FeatureRandom random{FeatureRandom::Kind::Xoroshiro, seed};
        CHECK(bush->place(context, level, random, {0, 65, 0}));
        i32 leaves = 0;
        for (const auto& [where, state] : level.written()) {
            if (pack->block_name(pack->block_of(state)) != "minecraft:oak_leaves") {
                continue;
            }
            ++leaves;
            const i32 x = static_cast<i32>(where >> 40);
            const i32 z = sign_extend_z(where);
            const i32 y = static_cast<i32>(where & 0xFFFFF) - 64;
            if (x == 0 && z == 0 && y == 67) {
                ++tops;
            }
        }
        leaf_counts.insert(leaves);
    }
    CHECK(leaf_counts.size() > 1);
    CHECK(tops > 0);
    CHECK(tops < kSeeds);
}
