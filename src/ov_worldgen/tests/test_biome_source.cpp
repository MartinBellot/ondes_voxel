// Every biome in the table, and whether the world can actually contain it.
//
// A parity run only ever sees the biomes that happen to be near the places it
// looked; the first one saw thirteen of fifty-three. These tests ask the
// question that does not depend on where anyone looked: is the table complete,
// and is every box in it reachable?
//
// A box that another box swallows would name a biome the world can never
// produce, and no amount of generated terrain would reveal it — you would
// simply never find that biome and never know whether it was your bug or the
// seed's bad luck.

#include "ov/worldgen/biome_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <set>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path reports_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated";
}

}  // namespace

TEST_CASE("the overworld table holds every biome 1.20.1 puts in it",
          "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent; run tools/ov_datagen first");
    }
    auto source = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(source.has_value());

    const auto names = source->biomes();
    std::set<std::string_view> present(names.begin(), names.end());

    // The fifty-three the overworld's multi-noise source can produce. Written
    // out rather than counted, so that a table which silently lost one is a
    // failure with a name in it.
    constexpr std::array<const char*, 53> kExpected{
        "minecraft:badlands",           "minecraft:bamboo_jungle",
        "minecraft:beach",              "minecraft:birch_forest",
        "minecraft:cherry_grove",       "minecraft:cold_ocean",
        "minecraft:dark_forest",        "minecraft:deep_cold_ocean",
        "minecraft:deep_dark",          "minecraft:deep_frozen_ocean",
        "minecraft:deep_lukewarm_ocean", "minecraft:deep_ocean",
        "minecraft:desert",             "minecraft:dripstone_caves",
        "minecraft:eroded_badlands",    "minecraft:flower_forest",
        "minecraft:forest",             "minecraft:frozen_ocean",
        "minecraft:frozen_peaks",       "minecraft:frozen_river",
        "minecraft:grove",              "minecraft:ice_spikes",
        "minecraft:jagged_peaks",       "minecraft:jungle",
        "minecraft:lukewarm_ocean",     "minecraft:lush_caves",
        "minecraft:mangrove_swamp",     "minecraft:meadow",
        "minecraft:mushroom_fields",    "minecraft:ocean",
        "minecraft:old_growth_birch_forest", "minecraft:old_growth_pine_taiga",
        "minecraft:old_growth_spruce_taiga", "minecraft:plains",
        "minecraft:river",              "minecraft:savanna",
        "minecraft:savanna_plateau",    "minecraft:snowy_beach",
        "minecraft:snowy_plains",       "minecraft:snowy_slopes",
        "minecraft:snowy_taiga",        "minecraft:sparse_jungle",
        "minecraft:stony_peaks",        "minecraft:stony_shore",
        "minecraft:sunflower_plains",   "minecraft:swamp",
        "minecraft:taiga",              "minecraft:warm_ocean",
        "minecraft:windswept_forest",   "minecraft:windswept_gravelly_hills",
        "minecraft:windswept_hills",    "minecraft:windswept_savanna",
        "minecraft:wooded_badlands",
    };

    for (const char* name : kExpected) {
        CAPTURE(name);
        CHECK(present.contains(name));
    }
    CHECK(present.size() == kExpected.size());
}

TEST_CASE("every biome in the table is reachable", "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent");
    }
    auto source = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(source.has_value());

    // Probe the middle of every box. A biome that never comes back is one the
    // world cannot contain, whatever the seed.
    std::set<std::string_view> reachable;
    for (usize index = 0; index < source->entry_count(); ++index) {
        reachable.insert(source->biome_at(source->entry_centre(index)));
    }

    for (const std::string_view name : source->biomes()) {
        CAPTURE(name);
        CHECK(reachable.contains(name));
    }
    CHECK(reachable.size() == source->biome_count());
}

TEST_CASE("the nether and the end have their own tables", "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent");
    }
    // The nether is multi-noise like the overworld and has an exported table;
    // the end is not — it uses a fixed rule over the island noise, so there is
    // no file and asking for one must fail rather than return an empty table
    // that would put one biome everywhere.
    auto nether = BiomeSource::load(reports_root(), "nether");
    REQUIRE(nether.has_value());
    CHECK(nether->biome_count() == 5);

    auto missing = BiomeSource::load(reports_root(), "the_end");
    CHECK_FALSE(missing.has_value());
}

TEST_CASE("a climate outside every box still names a biome", "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent");
    }
    auto source = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(source.has_value());

    // The search is nearest-box, not containment, so a point far outside every
    // box still has an answer. Returning nothing there would leave holes in the
    // world at exactly the extremes the noise reaches least often.
    ClimatePoint extreme;
    extreme.coordinates = {20000, 20000, 20000, 20000, 20000, 20000, 0};
    CHECK_FALSE(source->biome_at(extreme).empty());

    ClimatePoint opposite;
    opposite.coordinates = {-20000, -20000, -20000, -20000, -20000, -20000, 0};
    CHECK_FALSE(source->biome_at(opposite).empty());
}
