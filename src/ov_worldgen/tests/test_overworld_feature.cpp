// The overworld's feature types beyond trees, ores and plant patches.
//
// What is checked here is what no screenshot shows: that a type now loads
// instead of being refused, that the selectors which were blocked by it load
// with it, and that a feature's draws do not depend on anything but its seed.
// Whether the *shape* is the game's is measured against probe worlds by
// tools/ov_features --probe --control, not asserted here.

#include "ov/worldgen/feature.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <string_view>

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

[[nodiscard]] bool have_data() {
    return std::filesystem::exists(pack_path()) && std::filesystem::is_directory(data_root());
}

/// Grass over dirt over stone, air above.
class FlatLevel final : public FeatureLevel {
public:
    FlatLevel(registry::BlockStateId grass, registry::BlockStateId dirt,
              registry::BlockStateId stone, i32 surface)
        : grass_(grass), dirt_(dirt), stone_(stone), surface_(surface) {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto found = written_.find(key(x, y, z)); found != written_.end()) {
            return found->second;
        }
        if (y > surface_) return registry::kAirState;
        if (y == surface_) return grass_;
        return y > surface_ - 4 ? dirt_ : stone_;
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        if (outside_build_height(y)) return false;
        written_[key(x, y, z)] = state;
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType, i32, i32) const override { return surface_ + 1; }
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override { return "minecraft:plains"; }
    [[nodiscard]] i32 min_y() const override { return -64; }
    [[nodiscard]] i32 world_height() const override { return 384; }
    [[nodiscard]] i32 sea_level() const override { return 63; }

    [[nodiscard]] const std::map<i64, registry::BlockStateId>& written() const { return written_; }

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

class Everywhere final : public BiomeFeatures {
public:
    [[nodiscard]] bool lists(std::string_view, std::string_view) const override { return true; }
};

}  // namespace

TEST_CASE("the overworld's new feature types load, and what they blocked loads with them",
          "[worldgen][feature]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    static constexpr std::array<std::string_view, 7> kConfigured{
        "minecraft:huge_brown_mushroom", "minecraft:huge_red_mushroom",
        "minecraft:amethyst_geode",      "minecraft:dripstone_cluster",
        "minecraft:large_dripstone",     "minecraft:kelp",
        "minecraft:seagrass_tall",
    };
    for (const std::string_view name : kConfigured) {
        INFO(name);
        CHECK(registry->configured(name) != nullptr);
    }

    // Selectors and patches that were refused because one of their parts was.
    static constexpr std::array<std::string_view, 5> kUnblocked{
        "minecraft:dark_forest_vegetation",  // the two huge mushrooms
        "minecraft:mushroom_island_vegetation",
        "minecraft:pointed_dripstone",       // `solid`, through environment_scan
        "minecraft:patch_melon",             // `replaceable`
        "minecraft:warm_ocean_vegetation",   // the three corals
    };
    for (const std::string_view name : kUnblocked) {
        INFO(name);
        CHECK(registry->configured(name) != nullptr);
    }

    // Placed features whose *placement* or provider was the blocker:
    //   `forest_flowers`     — a `clamped` count whose source sits inside "value"
    //   `patch_sugar_cane`   — `matching_fluids` naming `flowing_water`
    //   `flower_plains`      — `noise_threshold_count` and `noise_threshold_provider`
    //   `kelp_warm`          — `noise_based_count`
    //   `bamboo`             — the feature type itself, under `noise_based_count`
    static constexpr std::array<std::string_view, 5> kPlaced{
        "minecraft:forest_flowers", "minecraft:patch_sugar_cane", "minecraft:flower_plains",
        "minecraft:kelp_warm",      "minecraft:bamboo",
    };
    for (const std::string_view name : kPlaced) {
        INFO(name);
        CHECK(registry->placed(name) != nullptr);
    }

    // 113 configured and 134 placed before this work; 150 and 183 after it.
    // The two mushroom patches stay refused: their survival reads light.
    CHECK(registry->configured_count() >= 150);
    CHECK(registry->placed_count() >= 180);
}

TEST_CASE("a huge mushroom is a stem and a cap, and its shape is its seed's",
          "[worldgen][feature]") {
    if (!have_data()) {
        SUCCEED("no generated data; run tools/ov_datagen");
        return;
    }
    auto pack = registry::BlockRegistry::load(pack_path());
    REQUIRE(pack.has_value());
    auto registry = FeatureRegistry::load(data_root(), *pack);
    REQUIRE(registry.has_value());

    const auto* brown = registry->configured("minecraft:huge_brown_mushroom");
    REQUIRE(brown != nullptr);
    const auto grass = pack->find_block("minecraft:grass_block");
    const auto dirt  = pack->find_block("minecraft:dirt");
    const auto stone = pack->find_block("minecraft:stone");
    const auto stem  = pack->find_block("minecraft:mushroom_stem");
    const auto cap   = pack->find_block("minecraft:brown_mushroom_block");
    REQUIRE(grass.has_value());
    REQUIRE(dirt.has_value());
    REQUIRE(stone.has_value());
    REQUIRE(stem.has_value());
    REQUIRE(cap.has_value());

    const Everywhere everywhere;
    FeatureContext   context;
    context.blocks = &*pack;
    context.biomes = &everywhere;

    const auto grow = [&](i64 seed) {
        FlatLevel level{pack->default_state(*grass), pack->default_state(*dirt),
                        pack->default_state(*stone), 70};
        FeatureRandom random{FeatureRandom::Kind::Xoroshiro, seed};
        REQUIRE(brown->place(context, level, random, {8, 71, 8}));
        return level.written();
    };

    for (i64 seed = 1; seed <= 20; ++seed) {
        const auto a = grow(seed);
        const auto b = grow(seed);
        CHECK(a == b);

        i32 stems = 0;
        i32 caps  = 0;
        for (const auto& [where, state] : a) {
            (void)where;
            if (pack->block_of(state) == *stem) ++stems;
            if (pack->block_of(state) == *cap) ++caps;
        }
        // Height `nextInt(3) + 4`, doubled one time in twelve.
        const bool plain   = stems >= 4 && stems <= 6;
        const bool doubled = stems >= 8 && stems <= 12 && stems % 2 == 0;
        CHECK((plain || doubled));
        // A 7-by-7 square at radius 3 with its four corners cut.
        CHECK(caps == 7 * 7 - 4);
    }
}
