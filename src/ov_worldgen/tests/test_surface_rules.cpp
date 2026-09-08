// The surface rules, loaded from the real vanilla data.
//
// What a unit test can settle here is narrower than it looks, and the narrow
// part matters: that the *whole* vanilla rule tree loads means every rule and
// condition type the overworld, nether and end use is implemented, every block
// it names is in the registry, and every noise it samples has a file. An
// unimplemented type is refused rather than treated as "never matches", so this
// test failing is how a missing condition is found instead of shipping a world
// with no beaches.
//
// What the values are worth is not settled here. The oracle for that is a world
// the real game wrote for the same seed: tools/ov_surfparity compares column by
// column against run/reference-1234567890 and reports a percentage per biome.
// See docs/provenance/surface-rules.md for the numbers.

#include "ov/worldgen/surface_rules.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

[[nodiscard]] std::filesystem::path registry_pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
           "registry.ovpack";
}

constexpr i64 kSeed = 1234567890;

}  // namespace

TEST_CASE("a vertical anchor resolves the three ways the datapack writes one",
          "[worldgen][surface]") {
    // The overworld is 384 blocks starting at -64, and the bedrock floor and
    // the deepslate transition are written in two different forms. Getting
    // either backwards moves a layer the whole world sits on.
    CHECK(resolve_anchor_absolute(0) == 0);
    CHECK(resolve_anchor_above_bottom(0, -64) == -64);
    CHECK(resolve_anchor_above_bottom(5, -64) == -59);
    CHECK(resolve_anchor_below_top(0, -64, 384) == 319);
    CHECK(resolve_anchor_below_top(5, -64, 384) == 314);
}

TEST_CASE("the clay bands are 192 and never all one colour", "[worldgen][surface]") {
    // The colours here are stand-ins: what is checked is the *shape* of the
    // draw, which is what the order of the RNG calls decides. Whether the table
    // is the game's table is a question only the game can answer, and
    // tools/ov_surfparity asks it.
    ClayBandColours colours;
    colours.terracotta = registry::BlockStateId{1};
    colours.orange     = registry::BlockStateId{2};
    colours.yellow     = registry::BlockStateId{3};
    colours.brown      = registry::BlockStateId{4};
    colours.red        = registry::BlockStateId{5};
    colours.white      = registry::BlockStateId{6};
    colours.light_gray = registry::BlockStateId{7};

    math::XoroshiroRandomSource random{kSeed};
    const auto                  bands = generate_clay_bands(random, colours);

    REQUIRE(bands.size() == 192);

    std::map<u16, usize> census;
    for (const auto band : bands) {
        ++census[band.value()];
    }
    // All seven colours appear. A table missing one is the symptom of a draw
    // consumed in the wrong order, which is otherwise invisible.
    CHECK(census.size() == 7);
    // Plain terracotta is the background and stays the most common.
    CHECK(census[colours.terracotta.value()] > census[colours.white.value()]);

    // Same seed, same table. The bands are drawn once per world and read from
    // every badlands column, so a table that varied would be a different
    // mountain each chunk.
    math::XoroshiroRandomSource again{kSeed};
    CHECK(generate_clay_bands(again, colours) == bands);
}

TEST_CASE("the whole vanilla surface rule loads", "[worldgen][surface]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent; run tools/ov_datagen first");
    }
    if (!std::filesystem::is_regular_file(registry_pack())) {
        SKIP("registry.ovpack absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());

    // Every settings file that has a surface rule. The nether and the end are
    // included on purpose: they exercise `y_above` with `below_top` anchors and
    // the ceiling side of `stone_depth`, which the overworld barely touches.
    for (const std::string settings : {"overworld", "large_biomes", "amplified", "nether", "end",
                                       "caves", "floating_islands"}) {
        INFO("settings: " << settings);
        auto system = SurfaceSystem::load(data_root(), settings, kSeed, *blocks);
        REQUIRE(system.has_value());
        CHECK(system->default_block() != registry::kAirState);
    }
}

TEST_CASE("the overworld's shape and bands come from the data", "[worldgen][surface]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen") ||
        !std::filesystem::is_regular_file(registry_pack())) {
        SKIP("vanilla data absent");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto system = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(system.has_value());

    CHECK(system->min_y() == -64);
    CHECK(system->height() == 384);
    CHECK(system->sea_level() == 63);

    const auto stone = blocks->find_block("minecraft:stone");
    REQUIRE(stone.has_value());
    CHECK(system->default_block() == blocks->default_state(*stone));

    // The band at a height is one of the seven terracottas and nothing else. A
    // band that came back as air would mean the offset noise pushed the index
    // out of the table — the modulo of a negative y is the trap, and two thirds
    // of the world has one.
    const auto terracotta = blocks->find_block("minecraft:terracotta");
    REQUIRE(terracotta.has_value());
    for (i32 y = -64; y < 320; ++y) {
        const auto band = system->clay_band(0, y, 0);
        INFO("y = " << y);
        REQUIRE(band != registry::kAirState);
        const auto name = blocks->block_name(blocks->block_of(band));
        REQUIRE(name.find("terracotta") != std::string_view::npos);
    }
}

TEST_CASE("the surface depth stays in the range the rules assume", "[worldgen][surface]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen") ||
        !std::filesystem::is_regular_file(registry_pack())) {
        SKIP("vanilla data absent");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto system = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(system.has_value());

    // The noise is scaled by 2.75 and offset by 3, so a depth outside about
    // 0..6 means the noise itself is wrong rather than the formula. Holes —
    // depth zero or less — are what the `hole` condition exists for, so they
    // must occur, and they must be rare.
    usize holes = 0;
    usize deep  = 0;
    for (i32 z = 0; z < 200; ++z) {
        for (i32 x = 0; x < 200; ++x) {
            const i32 depth = system->surface_depth(x * 7, z * 7);
            REQUIRE(depth >= -2);
            REQUIRE(depth <= 8);
            if (depth <= 0) {
                ++holes;
            }
            if (depth >= 5) {
                ++deep;
            }
        }
    }
    CHECK(holes > 0);
    CHECK(holes < 40000 / 8);
    CHECK(deep > 0);
}

namespace {

/// A world made up for the test: one biome, a flat surface, no cliffs.
///
/// The point of an abstract SurfaceQueries is exactly this — the rules can be
/// run over a column nobody generated, so the column walk can be checked
/// without the density graph in the way.
class FlatQueries final : public SurfaceQueries {
public:
    FlatQueries(std::string biome, i32 top) : biome_(std::move(biome)), top_(top) {}

    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override { return biome_; }
    [[nodiscard]] f64              temperature_at(i32, i32, i32) const override { return 0.8; }
    [[nodiscard]] i32              surface_height(i32, i32) const override { return top_; }
    [[nodiscard]] i32              preliminary_surface(i32, i32) const override { return -64; }

private:
    std::string biome_;
    i32         top_;
};

}  // namespace

TEST_CASE("a plains column gets grass, then dirt, then stone, over bedrock",
          "[worldgen][surface]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen") ||
        !std::filesystem::is_regular_file(registry_pack())) {
        SKIP("vanilla data absent");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto system = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(system.has_value());

    const auto name_of = [&](registry::BlockStateId state) {
        return state == registry::kAirState ? std::string_view("minecraft:air")
                                            : blocks->block_name(blocks->block_of(state));
    };

    constexpr i32                       kTop = 80;
    std::vector<registry::BlockStateId> column(384, registry::kAirState);
    for (i32 y = -64; y <= kTop; ++y) {
        column[static_cast<usize>(y + 64)] = system->default_block();
    }

    const FlatQueries queries{"minecraft:plains", kTop};
    system->build_column(column, 100, 100, queries, *blocks);

    // The floor. y = -64 is always bedrock and y = -59 never is; in between it
    // is a positional draw, so the test asserts the two certainties and that
    // the band holds nothing but bedrock and the deepslate the gradient below
    // it puts there.
    CHECK(name_of(column[0]) == "minecraft:bedrock");
    CHECK(name_of(column[5]) != "minecraft:bedrock");
    for (usize slot = 0; slot < 5; ++slot) {
        const auto here = name_of(column[slot]);
        INFO("y = " << static_cast<i32>(slot) - 64);
        CHECK((here == "minecraft:bedrock" || here == "minecraft:deepslate"));
    }

    // Deep stone is deepslate: the gradient is certain at and below y = 0 and
    // certainly not at y = 8.
    CHECK(name_of(column[static_cast<usize>(-1 + 64)]) == "minecraft:deepslate");
    CHECK(name_of(column[static_cast<usize>(8 + 64)]) == "minecraft:stone");

    // The surface. Grass on top, then as many blocks of dirt as the column's
    // own surface depth says, then stone — which is what
    // `stone_depth(floor, add_surface_depth)` means, and the reason the depth
    // is exposed at all.
    CHECK(name_of(column[static_cast<usize>(kTop + 64)]) == "minecraft:grass_block");
    const i32 depth = system->surface_depth(100, 100);
    REQUIRE(depth > 0);
    for (i32 step = 1; step <= depth; ++step) {
        INFO("dirt at y = " << kTop - step);
        CHECK(name_of(column[static_cast<usize>(kTop - step + 64)]) == "minecraft:dirt");
    }
    CHECK(name_of(column[static_cast<usize>(kTop - depth - 1 + 64)]) == "minecraft:stone");
}

TEST_CASE("a desert column gets sand over sandstone, and a badlands one gets bands",
          "[worldgen][surface]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen") ||
        !std::filesystem::is_regular_file(registry_pack())) {
        SKIP("vanilla data absent");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto system = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(system.has_value());

    const auto name_of = [&](registry::BlockStateId state) {
        return state == registry::kAirState ? std::string_view("minecraft:air")
                                            : blocks->block_name(blocks->block_of(state));
    };
    const auto build = [&](std::string_view biome, i32 top) {
        std::vector<registry::BlockStateId> column(384, registry::kAirState);
        for (i32 y = -64; y <= top; ++y) {
            column[static_cast<usize>(y + 64)] = system->default_block();
        }
        const FlatQueries queries{std::string(biome), top};
        system->build_column(column, 4096, 4096, queries, *blocks);
        return column;
    };

    {
        const auto column = build("minecraft:desert", 90);
        CHECK(name_of(column[static_cast<usize>(90 + 64)]) == "minecraft:sand");
        // Sandstone follows the sand, and it reaches further than the sand does
        // — that is the `secondary_depth_range` of 30, and a desert with no
        // sandstone under it is how a missing one would show.
        bool sandstone = false;
        for (i32 y = 89; y > 60; --y) {
            if (name_of(column[static_cast<usize>(y + 64)]) == "minecraft:sandstone") {
                sandstone = true;
                break;
            }
        }
        CHECK(sandstone);
    }
    {
        // Above sea level the badlands are terracotta bands, not sand.
        const auto column = build("minecraft:badlands", 100);
        usize      bands  = 0;
        for (i32 y = 100; y > 64; --y) {
            if (name_of(column[static_cast<usize>(y + 64)]).find("terracotta") !=
                std::string_view::npos) {
                ++bands;
            }
        }
        CHECK(bands > 20);
    }
}
