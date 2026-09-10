// Plants, against a hash-map world. Every rule here takes a `LevelWriter&`, so
// none of it needs a server — that is the point of the interface.
//
// The maps and thresholds asserted below are the ones measured on a real
// 1.20.1 server (docs/provenance/agriculture.md): the 9x9 hydration square,
// the leaf line that keeps six and drops the rest, ice at 10 against 11.
#include "ov/gameplay/plants.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        Packs out;
        auto  blocks = registry::BlockRegistry::load(path);
        auto  regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] const registry::BlockRegistry& blocks() { return *packs().blocks; }

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view name, std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const auto id = blocks().find_block(name);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks().default_state(*id);
    for (const auto& [key, value] : props) {
        const auto property = blocks().find_property(*id, key);
        REQUIRE(property.has_value());
        bool found = false;
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks().with_property(state, *property, static_cast<u16>(i));
                found = true;
            }
        }
        REQUIRE(found);
    }
    return state;
}

[[nodiscard]] std::string name_at(registry::BlockStateId state) {
    return std::string{blocks().block_name(blocks().block_of(state))};
}

[[nodiscard]] std::string value_at(registry::BlockStateId state, std::string_view property) {
    const auto p = blocks().find_property(blocks().block_of(state), property);
    return p ? std::string{blocks().property_value(state, *p)} : std::string{};
}

class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return {}; }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }

    void set_block(BlockPos pos, registry::BlockStateId state) override {
        cells_[key(pos)] = state;
        writes.push_back(pos);
    }
    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue,
                       world::TickPriority) override {
        scheduled.emplace_back(pos, std::string{what}, delay);
    }
    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue) const override {
        for (const auto& [where, name, _] : scheduled) {
            if (where == pos && name == what) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

    std::vector<BlockPos>                               writes;
    std::vector<std::tuple<BlockPos, std::string, i64>> scheduled;

private:
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos p) { return {p.x, p.y, p.z}; }
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> cells_;
};

class FakeEnv final : public PlantEnvironment {
public:
    u8 block{0};
    u8 sky{15};
    u8 darken{0};

    [[nodiscard]] u8   block_light(BlockPos) const override { return block; }
    [[nodiscard]] u8   sky_light(BlockPos) const override { return sky; }
    [[nodiscard]] u8   sky_darken() const override { return darken; }
    [[nodiscard]] bool is_raining_at(BlockPos) const override { return false; }
    void drop_block(BlockPos pos, registry::BlockStateId state) override {
        dropped.emplace_back(pos, state);
    }
    bool grow_tree(world::LevelWriter&, BlockPos pos, registry::BlockStateId,
                   PlantRandom&) override {
        trees.push_back(pos);
        return true;
    }

    std::vector<std::pair<BlockPos, registry::BlockStateId>> dropped;
    std::vector<BlockPos>                                    trees;
};

[[nodiscard]] const Plants& plants() {
    static const Plants rules{*packs().blocks, *packs().registries};
    return rules;
}

/// A wet tile like the measured ones: 9x9 moist farmland, water in the middle.
void wet_tile(MapLevel& level, BlockPos centre) {
    for (i32 dz = -4; dz <= 4; ++dz) {
        for (i32 dx = -4; dx <= 4; ++dx) {
            level.set_block(centre.offset(dx, 0, dz), state_of("minecraft:farmland", {{"moisture", "7"}}));
        }
    }
    level.set_block(centre, state_of("minecraft:water"));
}

}  // namespace

TEST_CASE("the growth speed is the documented points formula", "[gameplay][plants]") {
    REQUIRE(packs().blocks.has_value());
    MapLevel level;
    wet_tile(level, {0, 0, 0});

    // A lone crop, eight moist neighbours: 1 + 3 + 8 x 0.75 = 10.
    level.set_block({2, 1, 2}, state_of("minecraft:wheat"));
    REQUIRE(plants().growth_speed(level, {2, 1, 2}) == 10.0F);

    // Next to the water, seven moist neighbours: 9.25 — and still one in three,
    // because 25 / 9.25 truncates to 2 as 25 / 10 does.
    level.set_block({1, 1, 1}, state_of("minecraft:wheat"));
    // ...but now the two wheat are diagonal to each other, which halves both.
    REQUIRE(plants().growth_speed(level, {2, 1, 2}) == 5.0F);
    REQUIRE(plants().growth_speed(level, {1, 1, 1}) == 4.625F);

    // A different crop on the diagonal does not halve.
    level.set_block({1, 1, 1}, state_of("minecraft:carrots"));
    REQUIRE(plants().growth_speed(level, {2, 1, 2}) == 10.0F);

    // Same crop in one axis only: no halving. In both axes: halved.
    level.set_block({1, 1, 1}, registry::kAirState);
    level.set_block({3, 1, 2}, state_of("minecraft:wheat"));
    REQUIRE(plants().growth_speed(level, {2, 1, 2}) == 10.0F);
    level.set_block({2, 1, 3}, state_of("minecraft:wheat"));
    REQUIRE(plants().growth_speed(level, {2, 1, 2}) == 5.0F);

    // Dry rows: 1 + 1 + 8 x 0.25 = 4.
    MapLevel dry;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            dry.set_block({dx, 0, dz}, state_of("minecraft:farmland"));
        }
    }
    dry.set_block({0, 1, 0}, state_of("minecraft:wheat"));
    dry.set_block({1, 1, 0}, state_of("minecraft:wheat"));
    dry.set_block({0, 1, 1}, state_of("minecraft:carrots"));
    REQUIRE(plants().growth_speed(dry, {0, 1, 0}) == 4.0F);
}

TEST_CASE("farmland is hydrated exactly in the measured square", "[gameplay][plants]") {
    // Measured: a 9x9 square around the water, the water at the farmland's
    // level or one above; one below and two above hydrate nothing.
    for (const i32 water_dy : {-1, 0, 1, 2}) {
        MapLevel level;
        level.set_block({0, water_dy, 0}, state_of("minecraft:water"));
        usize hydrated = 0;
        for (i32 dz = -6; dz <= 6; ++dz) {
            for (i32 dx = -6; dx <= 6; ++dx) {
                const bool wet    = plants().farmland_near_water(level, {dx, 0, dz});
                const bool expect = (water_dy == 0 || water_dy == 1) && std::abs(dx) <= 4 &&
                                    std::abs(dz) <= 4;
                REQUIRE(wet == expect);
                hydrated += wet ? 1 : 0;
            }
        }
        REQUIRE(hydrated == ((water_dy == 0 || water_dy == 1) ? 81U : 0U));
    }
}

TEST_CASE("dry farmland without a crop turns back to dirt, with one it only dries",
          "[gameplay][plants]") {
    MapLevel    level;
    FakeEnv     env;
    PlantRandom random{1};
    level.set_block({0, 0, 0}, state_of("minecraft:farmland", {{"moisture", "1"}}));
    plants().random_tick(level, env, {0, 0, 0}, level.block_at({0, 0, 0}), random);
    REQUIRE(value_at(level.block_at({0, 0, 0}), "moisture") == "0");
    plants().random_tick(level, env, {0, 0, 0}, level.block_at({0, 0, 0}), random);
    REQUIRE(name_at(level.block_at({0, 0, 0})) == "minecraft:dirt");

    level.set_block({5, 0, 0}, state_of("minecraft:farmland"));
    level.set_block({5, 1, 0}, state_of("minecraft:wheat"));
    plants().random_tick(level, env, {5, 0, 0}, level.block_at({5, 0, 0}), random);
    REQUIRE(name_at(level.block_at({5, 0, 0})) == "minecraft:farmland");

    // Water in reach: straight to 7.
    level.set_block({9, 0, 3}, state_of("minecraft:water"));
    plants().random_tick(level, env, {5, 0, 0}, level.block_at({5, 0, 0}), random);
    REQUIRE(value_at(level.block_at({5, 0, 0}), "moisture") == "7");
}

TEST_CASE("leaf distance follows #logs through any leaves, and seven decays",
          "[gameplay][plants]") {
    FakeEnv     env;
    PlantRandom random{7};
    for (const std::string_view source : {"minecraft:oak_log", "minecraft:stripped_oak_log",
                                          "minecraft:oak_wood", "minecraft:crimson_stem"}) {
        MapLevel level;
        level.set_block({0, 0, 0}, state_of(source));
        const std::array<std::string_view, 3> kinds{"minecraft:oak_leaves", "minecraft:birch_leaves",
                                                    "minecraft:cherry_leaves"};
        // Placed outward one at a time, each asked its distance as it lands —
        // what `setblock` does, and what the measurement did.
        for (i32 i = 1; i <= 10; ++i) {
            const auto leaf = state_of(kinds[static_cast<usize>(i) % 3], {{"persistent", "false"}});
            level.set_block({i, 0, 0}, leaf);
            const i32 d = plants().leaf_distance(level, {i, 0, 0});
            level.set_block({i, 0, 0}, state_of(kinds[static_cast<usize>(i) % 3],
                                                {{"persistent", "false"},
                                                 {"distance", std::to_string(d)}}));
        }
        for (i32 i = 1; i <= 10; ++i) {
            const auto here = level.block_at({i, 0, 0});
            REQUIRE(plants().ticks_randomly(here) == (i >= 7));
            plants().random_tick(level, env, {i, 0, 0}, here, random);
            REQUIRE((name_at(level.block_at({i, 0, 0})) == "minecraft:air") == (i >= 7));
        }
    }

    // Not a log: planks and mangrove roots hold nothing.
    for (const std::string_view source : {"minecraft:oak_planks", "minecraft:mangrove_roots"}) {
        MapLevel level;
        level.set_block({0, 0, 0}, state_of(source));
        level.set_block({1, 0, 0}, state_of("minecraft:oak_leaves"));
        REQUIRE(plants().leaf_distance(level, {1, 0, 0}) == 7);
    }

    // A diagonal touch counts for nothing.
    MapLevel diagonal;
    diagonal.set_block({0, 0, 0}, state_of("minecraft:oak_log"));
    diagonal.set_block({1, 0, 1}, state_of("minecraft:oak_leaves"));
    REQUIRE(plants().leaf_distance(diagonal, {1, 0, 1}) == 7);

    // Persistent leaves never tick.
    REQUIRE_FALSE(plants().ticks_randomly(state_of("minecraft:oak_leaves", {{"persistent", "true"}})));
}

TEST_CASE("a leaf told about a lost log asks for a tick and then recomputes",
          "[gameplay][plants]") {
    MapLevel level;
    FakeEnv  env;
    level.set_block({0, 0, 0}, state_of("minecraft:oak_log"));
    level.set_block({1, 0, 0}, state_of("minecraft:oak_leaves", {{"persistent", "false"}, {"distance", "1"}}));
    level.set_block({0, 0, 0}, registry::kAirState);
    plants().neighbour_changed(level, env, {1, 0, 0}, {0, 0, 0});
    REQUIRE(level.scheduled.size() == 1);
    REQUIRE(std::get<1>(level.scheduled[0]) == "minecraft:oak_leaves");
    REQUIRE(std::get<2>(level.scheduled[0]) == 1);
    const auto leaves = blocks().find_block("minecraft:oak_leaves");
    REQUIRE(plants().scheduled_tick(level, env, {1, 0, 0}, *leaves));
    REQUIRE(value_at(level.block_at({1, 0, 0}), "distance") == "7");
}

TEST_CASE("ice and snow melt at the measured block light", "[gameplay][plants]") {
    PlantRandom random{3};
    for (const u8 light : {u8{10}, u8{11}}) {
        MapLevel level;
        FakeEnv  env;
        env.block = light;
        level.set_block({0, 0, 0}, state_of("minecraft:ice"));
        plants().random_tick(level, env, {0, 0, 0}, level.block_at({0, 0, 0}), random);
        REQUIRE(name_at(level.block_at({0, 0, 0})) == (light >= 11 ? "minecraft:water" : "minecraft:ice"));
    }
    for (const u8 light : {u8{11}, u8{12}}) {
        MapLevel level;
        FakeEnv  env;
        env.block = light;
        level.set_block({0, 0, 0}, state_of("minecraft:snow"));
        plants().random_tick(level, env, {0, 0, 0}, level.block_at({0, 0, 0}), random);
        REQUIRE(name_at(level.block_at({0, 0, 0})) == (light >= 12 ? "minecraft:air" : "minecraft:snow"));
    }
}

TEST_CASE("bone meal on wheat adds two to five stages, uniformly", "[gameplay][plants]") {
    std::array<usize, 8> counts{};
    for (i64 seed = 0; seed < 4000; ++seed) {
        MapLevel    level;
        FakeEnv     env;
        PlantRandom random{seed};
        level.set_block({0, 0, 0}, state_of("minecraft:farmland"));
        level.set_block({0, 1, 0}, state_of("minecraft:wheat"));
        const UseOutcome out = plants().bone_meal(level, env, {0, 1, 0}, random);
        REQUIRE(out.result == UseResult::Success);
        REQUIRE(out.consume_one);
        ++counts[static_cast<usize>(std::stoi(value_at(level.block_at({0, 1, 0}), "age")))];
    }
    REQUIRE(counts[0] + counts[1] + counts[6] + counts[7] == 0);
    for (usize age = 2; age <= 5; ++age) {
        REQUIRE(counts[age] > 900);
        REQUIRE(counts[age] < 1100);
    }

    // A mature crop does not take bone meal, and none is used.
    MapLevel    level;
    FakeEnv     env;
    PlantRandom random{9};
    level.set_block({0, 1, 0}, state_of("minecraft:wheat", {{"age", "7"}}));
    REQUIRE(plants().bone_meal(level, env, {0, 1, 0}, random).result == UseResult::Pass);
}

TEST_CASE("bone meal on a sapling spends itself and advances 45 % of the time",
          "[gameplay][plants]") {
    usize advanced = 0;
    for (i64 seed = 0; seed < 4000; ++seed) {
        MapLevel    level;
        FakeEnv     env;
        PlantRandom random{seed};
        level.set_block({0, 1, 0}, state_of("minecraft:oak_sapling"));
        const UseOutcome out = plants().bone_meal(level, env, {0, 1, 0}, random);
        REQUIRE(out.consume_one);
        advanced += value_at(level.block_at({0, 1, 0}), "stage") == "1" ? 1 : 0;
    }
    REQUIRE(advanced > 1700);
    REQUIRE(advanced < 1900);

    // A stage-1 sapling grows a tree through the environment.
    MapLevel    level;
    FakeEnv     env;
    PlantRandom random{0};
    level.set_block({0, 1, 0}, state_of("minecraft:oak_sapling", {{"stage", "1"}}));
    for (i32 i = 0; i < 20 && env.trees.empty(); ++i) {
        (void)plants().bone_meal(level, env, {0, 1, 0}, random);
    }
    REQUIRE(env.trees.size() == 1);
}

TEST_CASE("a crop whose farmland is dug out breaks and drops", "[gameplay][plants]") {
    MapLevel level;
    FakeEnv  env;
    level.set_block({0, 0, 0}, state_of("minecraft:farmland"));
    level.set_block({0, 1, 0}, state_of("minecraft:wheat", {{"age", "7"}}));
    plants().neighbour_changed(level, env, {0, 1, 0}, {0, 0, 0});
    REQUIRE(name_at(level.block_at({0, 1, 0})) == "minecraft:wheat");

    level.set_block({0, 0, 0}, state_of("minecraft:dirt"));
    plants().neighbour_changed(level, env, {0, 1, 0}, {0, 0, 0});
    REQUIRE(name_at(level.block_at({0, 1, 0})) == "minecraft:air");
    REQUIRE(env.dropped.size() == 1);
    REQUIRE(value_at(env.dropped[0].second, "age") == "7");
}

TEST_CASE("a mature stem sets a fruit beside it and bends towards it", "[gameplay][plants]") {
    usize fruits = 0;
    for (i64 seed = 0; seed < 200; ++seed) {
        MapLevel    level;
        FakeEnv     env;
        PlantRandom random{seed};
        wet_tile(level, {0, 0, 0});
        level.set_block({2, 1, 2}, state_of("minecraft:melon_stem", {{"age", "7"}}));
        for (i32 i = 0; i < 50; ++i) {
            plants().random_tick(level, env, {2, 1, 2}, level.block_at({2, 1, 2}), random);
        }
        const auto stem = level.block_at({2, 1, 2});
        if (name_at(stem) != "minecraft:attached_melon_stem") {
            continue;
        }
        ++fruits;
        const std::string facing = value_at(stem, "facing");
        const BlockPos    fruit  = facing == "north"   ? BlockPos{2, 1, 1}
                                   : facing == "south" ? BlockPos{2, 1, 3}
                                   : facing == "east"  ? BlockPos{3, 1, 2}
                                                       : BlockPos{1, 1, 2};
        REQUIRE(name_at(level.block_at(fruit)) == "minecraft:melon");

        // Take the melon: the stem straightens at age 7.
        level.set_block(fruit, registry::kAirState);
        plants().neighbour_changed(level, env, {2, 1, 2}, fruit);
        REQUIRE(name_at(level.block_at({2, 1, 2})) == "minecraft:melon_stem");
        REQUIRE(value_at(level.block_at({2, 1, 2}), "age") == "7");
    }
    REQUIRE(fruits > 150);
}

TEST_CASE("sugar cane grows to three and no further", "[gameplay][plants]") {
    MapLevel    level;
    FakeEnv     env;
    PlantRandom random{5};
    level.set_block({0, 0, 0}, state_of("minecraft:sand"));
    level.set_block({1, 0, 0}, state_of("minecraft:water"));
    level.set_block({0, 1, 0}, state_of("minecraft:sugar_cane"));
    for (i32 i = 0; i < 200; ++i) {
        for (i32 y = 1; y <= 4; ++y) {
            const auto here = level.block_at({0, y, 0});
            if (plants().ticks_randomly(here)) {
                plants().random_tick(level, env, {0, y, 0}, here, random);
            }
        }
    }
    REQUIRE(name_at(level.block_at({0, 3, 0})) == "minecraft:sugar_cane");
    REQUIRE(name_at(level.block_at({0, 4, 0})) == "minecraft:air");

    // Without water it uproots.
    level.set_block({1, 0, 0}, state_of("minecraft:sand"));
    plants().random_tick(level, env, {0, 1, 0}, level.block_at({0, 1, 0}), random);
    REQUIRE(name_at(level.block_at({0, 1, 0})) == "minecraft:air");
}

TEST_CASE("a cactus next to a solid block asks to be broken", "[gameplay][plants]") {
    MapLevel level;
    FakeEnv  env;
    level.set_block({0, 0, 0}, state_of("minecraft:sand"));
    level.set_block({0, 1, 0}, state_of("minecraft:cactus"));
    level.set_block({1, 1, 0}, state_of("minecraft:stone"));
    plants().neighbour_changed(level, env, {0, 1, 0}, {1, 1, 0});
    REQUIRE(level.scheduled.size() == 1);
    const auto cactus = blocks().find_block("minecraft:cactus");
    REQUIRE(plants().scheduled_tick(level, env, {0, 1, 0}, *cactus));
    REQUIRE(name_at(level.block_at({0, 1, 0})) == "minecraft:air");
    REQUIRE(env.dropped.size() == 1);
}

TEST_CASE("grass dies under a lid and spreads onto lit dirt", "[gameplay][plants]") {
    FakeEnv     env;
    PlantRandom random{11};
    MapLevel    covered;
    covered.set_block({0, 0, 0}, state_of("minecraft:grass_block"));
    covered.set_block({0, 1, 0}, state_of("minecraft:stone"));
    plants().random_tick(covered, env, {0, 0, 0}, covered.block_at({0, 0, 0}), random);
    REQUIRE(name_at(covered.block_at({0, 0, 0})) == "minecraft:dirt");

    MapLevel field;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            field.set_block({dx, 0, dz}, state_of("minecraft:dirt"));
        }
    }
    field.set_block({0, 0, 0}, state_of("minecraft:grass_block"));
    for (i32 i = 0; i < 100; ++i) {
        plants().random_tick(field, env, {0, 0, 0}, field.block_at({0, 0, 0}), random);
    }
    usize grass = 0;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            grass += name_at(field.block_at({dx, 0, dz})) == "minecraft:grass_block" ? 1 : 0;
        }
    }
    REQUIRE(grass == 9);

    // In the dark it does not spread.
    MapLevel dark;
    dark.set_block({0, 0, 0}, state_of("minecraft:grass_block"));
    dark.set_block({1, 0, 0}, state_of("minecraft:dirt"));
    FakeEnv night;
    night.sky    = 15;
    night.darken = 11;
    for (i32 i = 0; i < 200; ++i) {
        plants().random_tick(dark, night, {0, 0, 0}, dark.block_at({0, 0, 0}), random);
    }
    REQUIRE(name_at(dark.block_at({1, 0, 0})) == "minecraft:dirt");
}

TEST_CASE("seeds plant on farmland only, cocoa on the side of a jungle log",
          "[gameplay][plants]") {
    MapLevel level;
    level.set_block({0, 0, 0}, state_of("minecraft:farmland"));
    level.set_block({2, 0, 0}, state_of("minecraft:dirt"));
    REQUIRE(plants().is_plantable("minecraft:wheat_seeds"));
    REQUIRE_FALSE(plants().is_plantable("minecraft:stone"));

    const PlantOutcome on_farmland = plants().plant(level, {0, 0, 0}, 1, "minecraft:wheat_seeds");
    REQUIRE(on_farmland.planted);
    REQUIRE(name_at(level.block_at({0, 1, 0})) == "minecraft:wheat");
    REQUIRE_FALSE(plants().plant(level, {2, 0, 0}, 1, "minecraft:wheat_seeds").planted);
    REQUIRE(plants().plant(level, {2, 0, 0}, 1, "minecraft:oak_sapling").planted);

    level.set_block({5, 1, 5}, state_of("minecraft:jungle_log"));
    // Face 2 is north: the pod hangs at z - 1 and faces south, back at the log.
    const PlantOutcome pod = plants().plant(level, {5, 1, 5}, 2, "minecraft:cocoa_beans");
    REQUIRE(pod.planted);
    REQUIRE(pod.at == BlockPos{5, 1, 4});
    REQUIRE(value_at(level.block_at({5, 1, 4}), "facing") == "south");
    REQUIRE_FALSE(plants().plant(level, {5, 1, 5}, 1, "minecraft:cocoa_beans").planted);
}

TEST_CASE("bone meal on grass makes a patch the size of the real game's", "[gameplay][plants]") {
    // The real game, 64 patches on a flat grass field, one dispenser each
    // (§ poudre d'os sur l'herbe): 14.45 short grass per patch (sd 2.81), about
    // two tall grass, and by Chebyshev ring around the target the share of
    // cells holding grass or tall grass — flowers left out on both sides,
    // because this server does not place them (2.7 a patch in the game):
    // ring 0 92 %, ring 1 72 %, ring 2 36 %, ring 3 11 %, ring 4 3 %.
    constexpr i32 kPatches = 400;
    f64                   grass = 0.0;
    f64                   tall  = 0.0;
    std::array<f64, 8>    ring_hits{};
    for (i32 patch = 0; patch < kPatches; ++patch) {
        MapLevel    level;
        FakeEnv     env;
        PlantRandom random{patch * 7919 + 1};
        for (i32 dz = -9; dz <= 9; ++dz) {
            for (i32 dx = -9; dx <= 9; ++dx) {
                level.set_block({dx, 0, dz}, state_of("minecraft:grass_block"));
            }
        }
        // The oracle's rig, exactly: the dispenser is sunk into the ground
        // just south of the target and the redstone block that fired it just
        // south of that. Neither is grass, so nothing grows on them and a walk
        // that steps onto either stops — ring 1 can never be more than 7/8
        // full in the real measurement.
        level.set_block({0, 0, 1}, state_of("minecraft:dispenser"));
        level.set_block({0, 0, 2}, state_of("minecraft:redstone_block"));
        const UseOutcome out = plants().bone_meal(level, env, {0, 0, 0}, random);
        REQUIRE(out.result == UseResult::Success);
        for (i32 dz = -7; dz <= 7; ++dz) {
            for (i32 dx = -7; dx <= 7; ++dx) {
                const std::string here = name_at(level.block_at({dx, 1, dz}));
                if (here == "minecraft:air") {
                    continue;
                }
                grass += here == "minecraft:grass" ? 1.0 : 0.0;
                tall += here == "minecraft:tall_grass" ? 1.0 : 0.0;
                ring_hits[static_cast<usize>(std::max(std::abs(dx), std::abs(dz)))] += 1.0;
            }
        }
    }
    const f64 mean_grass = grass / kPatches;
    const f64 mean_tall  = tall / kPatches;
    INFO("grass " << mean_grass << " tall " << mean_tall << " ring1 "
                  << ring_hits[1] / (8.0 * kPatches) << " ring2 " << ring_hits[2] / (16.0 * kPatches)
                  << " ring3 " << ring_hits[3] / (24.0 * kPatches));
    // Three standard errors of the real game's 64-patch mean either side.
    REQUIRE(mean_grass > 14.45 - 3.0 * 2.81 / 8.0);
    REQUIRE(mean_grass < 14.45 + 3.0 * 2.81 / 8.0);
    REQUIRE(mean_tall > 1.0);
    REQUIRE(mean_tall < 3.0);
    // Bands of about three standard errors of the game's 64-patch fractions.
    REQUIRE(ring_hits[0] / kPatches > 0.78);
    REQUIRE(ring_hits[0] / kPatches < 0.99);
    REQUIRE(ring_hits[1] / (8.0 * kPatches) > 0.64);
    REQUIRE(ring_hits[1] / (8.0 * kPatches) < 0.80);
    REQUIRE(ring_hits[2] / (16.0 * kPatches) > 0.31);
    REQUIRE(ring_hits[2] / (16.0 * kPatches) < 0.42);
    REQUIRE(ring_hits[3] / (24.0 * kPatches) > 0.08);
    REQUIRE(ring_hits[3] / (24.0 * kPatches) < 0.15);
}

TEST_CASE("every unanswered ticker is named", "[gameplay][plants]") {
    REQUIRE(Plants::unanswered().size() > 10);
    for (const UnansweredTicker& t : Plants::unanswered()) {
        REQUIRE_FALSE(t.why.empty());
    }
    REQUIRE_FALSE(plants().ticks_randomly(state_of("minecraft:wheat", {{"age", "7"}})));
    REQUIRE(plants().ticks_randomly(state_of("minecraft:wheat")));
    REQUIRE_FALSE(plants().ticks_randomly(state_of("minecraft:stone")));
}
