#include "ov/render/biome_colours.hpp"

#include "ov/registry/block_states.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace ov;
using namespace ov::render;

namespace {

/// The published grass colour of a biome, and the climate it comes from.
///
/// These are the values the game shows, quoted independently of any code: if
/// the sampling rule, the axis order, the rainfall scaling or the two
/// modifiers are wrong in any way, the numbers here stop matching. That is the
/// whole reason to check colours by number rather than by looking at a
/// screenshot and calling it green.
struct Published {
    const char* biome;
    u32         grass;
};

constexpr std::array kPublishedGrass{
    Published{"minecraft:plains", 0x91BD59},
    Published{"minecraft:forest", 0x79C05A},
    Published{"minecraft:jungle", 0x59C93C},
    // Overridden by grass_color_modifier, not sampled.
    Published{"minecraft:swamp", 0x6A7039},
    // temperature 2.0, rainfall 0.0 — both clamped before sampling.
    Published{"minecraft:desert", 0xBFB755},
    // grass_color override in the biome file: the colormap is not consulted.
    Published{"minecraft:badlands", 0x90814D},
    Published{"minecraft:snowy_plains", 0x80B497},
    Published{"minecraft:taiga", 0x86B783},
    Published{"minecraft:savanna", 0xBFB755},
    Published{"minecraft:birch_forest", 0x88BB67},
    // The dark_forest modifier, applied on top of the sample.
    Published{"minecraft:dark_forest", 0x507A32},
    Published{"minecraft:ocean", 0x8EB971},
    Published{"minecraft:windswept_hills", 0x8AB689},
    Published{"minecraft:mushroom_fields", 0x55C93F},
    Published{"minecraft:old_growth_pine_taiga", 0x86B87F},
};

/// The pack and the registry are generated locally and never committed, so
/// every test here skips rather than fails when they are absent — a fresh
/// clone has no assets and must still go green.
[[nodiscard]] std::optional<std::pair<registry::BlockRegistry, DirectoryAssetSource>> environment() {
    const std::filesystem::path pack("data/vanilla/1.20.1/registry.ovpack");
    const std::filesystem::path assets("run/assets");
    if (!std::filesystem::exists(pack) || !std::filesystem::is_directory(assets / "assets")) {
        return std::nullopt;
    }
    auto registry = registry::BlockRegistry::load(pack);
    if (!registry) {
        return std::nullopt;
    }
    return std::pair{std::move(*registry), DirectoryAssetSource(assets)};
}

}  // namespace

TEST_CASE("fifteen published biome colours come back exactly", "[render][biome]") {
    auto env = environment();
    if (!env) {
        SKIP("registry.ovpack or run/assets is absent; generate them first");
    }
    auto& [registry, assets] = *env;

    auto colours = BiomeColours::load(assets, registry);
    REQUIRE(colours.has_value());
    REQUIRE(colours->size() == registry.biome_count());
    REQUIRE(registry.biome_count() == 64);

    for (const auto& expected : kPublishedGrass) {
        CAPTURE(expected.biome);
        const auto index = registry.find_biome(expected.biome);
        REQUIRE(index.has_value());
        CHECK(colours->grass(*index) == expected.grass);
    }
}

TEST_CASE("water is stored, not sampled", "[render][biome]") {
    auto env = environment();
    if (!env) {
        SKIP("registry.ovpack or run/assets is absent; generate them first");
    }
    auto& [registry, assets] = *env;
    auto colours             = BiomeColours::load(assets, registry);
    REQUIRE(colours.has_value());

    // Three biomes whose water differs, which is the only reason the field is
    // per biome rather than a constant.
    const auto plains = registry.find_biome("minecraft:plains");
    const auto swamp  = registry.find_biome("minecraft:swamp");
    const auto cherry = registry.find_biome("minecraft:cherry_grove");
    REQUIRE(plains.has_value());
    REQUIRE(swamp.has_value());
    REQUIRE(cherry.has_value());

    CHECK(colours->water(*plains) == 0x3F76E4);
    CHECK(colours->water(*swamp) == 0x617B64);
    CHECK(colours->water(*cherry) == 0x5DB7EF);
}

TEST_CASE("biome names are found by binary search and reported back", "[render][biome]") {
    auto env = environment();
    if (!env) {
        SKIP("registry.ovpack or run/assets is absent; generate them first");
    }
    auto& [registry, assets] = *env;

    // The section is sorted, so the first and last names are the extremes of
    // the search and the ones a broken bound would miss.
    const auto first = registry.find_biome(registry.biome_name(0));
    const auto last  = registry.find_biome(registry.biome_name(63));
    REQUIRE(first == 0U);
    REQUIRE(last == 63U);
    CHECK_FALSE(registry.find_biome("minecraft:not_a_biome").has_value());
    CHECK_FALSE(registry.find_biome("").has_value());
}

TEST_CASE("an out-of-range climate is magenta, not a plausible green", "[render][biome]") {
    // A 4x1 strip: sampling past it must not silently read a neighbouring
    // pixel or clamp into something that looks like grass.
    const std::array<u8, 16> pixels{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    // x = 255, y = 255 on a four-pixel strip: far past the end.
    CHECK(BiomeColours::sample_colormap(pixels, 4, 0.0, 0.0) == 0xFF00FF);
}
