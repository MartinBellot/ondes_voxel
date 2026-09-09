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
#include <limits>
#include <set>
#include <string_view>

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

namespace {

/// A cheap deterministic stream of climates, for probing the search.
///
/// Pseudo-random rather than a grid: a grid over six axes at any useful
/// resolution is billions of points, and what matters is the climates that
/// fall *between* boxes, which a coarse grid mostly misses.
class ClimateProbe {
public:
    explicit ClimateProbe(u64 seed) noexcept : state_(seed) {}

    [[nodiscard]] ClimatePoint next() noexcept {
        ClimatePoint point;
        for (usize axis = 0; axis < 6; ++axis) {
            point.coordinates[axis] = static_cast<i64>(draw() % 24001) - 12000;
        }
        // The seventh axis is never sampled by the world, so probing it with
        // anything but zero would ask a question the world cannot ask.
        point.coordinates[6] = 0;
        return point;
    }

    [[nodiscard]] u64 draw() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33;
    }

private:
    u64 state_;
};

}  // namespace

TEST_CASE("the tree finds the nearest box a scan would", "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent");
    }
    auto source = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(source.has_value());

    // The tree exists to decide *ties*, and it is allowed to name a different
    // box from a scan only when both are exactly as near. Anything else is a
    // biome in the wrong place, so this checks the distance rather than the
    // name: the minimum over every biome in the table, computed independently
    // of the tree.
    const auto names = source->biomes();
    ClimateProbe probe{0x9E3779B97F4A7C15ULL};
    for (usize round = 0; round < 4000; ++round) {
        const ClimatePoint point = probe.next();
        const auto         chosen = source->biome_at(point);
        REQUIRE_FALSE(chosen.empty());

        i64 nearest = std::numeric_limits<i64>::max();
        for (const std::string_view name : names) {
            nearest = std::min(nearest, source->distance_to(point, name));
        }
        CHECK(source->distance_to(point, chosen) == nearest);
    }
}

TEST_CASE("the search cache breaks ties and nothing else", "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent");
    }
    auto source = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(source.has_value());

    // Priming the cache with an arbitrary box must never make the search
    // return a box that is further away than the one it would have returned
    // cold. That is the cache's whole safety property: it can relabel a
    // boundary and it can never move a biome. A cache that could would be a
    // generator whose output depends on how many chunks were loaded first.
    ClimateProbe probe{0xD1B54A32D192ED03ULL};
    for (usize round = 0; round < 4000; ++round) {
        const ClimatePoint point = probe.next();
        const i64          cold  = source->distance_to(point, source->biome_at(point));

        BiomeSearchCache primed;
        primed.last = static_cast<i32>(probe.draw() % source->entry_count());
        const auto warm = source->biome_at(point, primed);
        REQUIRE_FALSE(warm.empty());
        CHECK(source->distance_to(point, warm) == cold);

        // And the cache now holds what it just returned, which is what makes
        // the next query's answer depend on this one.
        REQUIRE(primed.last >= 0);
        CHECK(source->entry_biome(static_cast<usize>(primed.last)) == warm);
    }
}

TEST_CASE("the cache actually changes an answer somewhere", "[worldgen][biome]") {
    if (!std::filesystem::is_directory(reports_root() / "reports")) {
        SKIP("vanilla reports absent");
    }
    auto source = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(source.has_value());

    // The complement of the case above, and the reason this whole mechanism is
    // in the code: a cache that never changed anything would be an
    // optimisation, and 2197 biome cells in a reference world say it is not.
    // If this ever stops finding a tie, either the table stopped overlapping
    // or the tie-break stopped being reachable, and either way the parity
    // number is about to move.
    ClimateProbe probe{0x2545F4914F6CDD1DULL};
    usize        changed = 0;
    for (usize round = 0; round < 20000 && changed == 0; ++round) {
        const ClimatePoint point = probe.next();
        const auto         cold  = source->biome_at(point);
        for (i32 entry = 0; entry < static_cast<i32>(source->entry_count()); ++entry) {
            BiomeSearchCache primed;
            primed.last = entry;
            if (source->biome_at(point, primed) != cold) {
                ++changed;
                break;
            }
        }
    }
    CHECK(changed != 0);
}
