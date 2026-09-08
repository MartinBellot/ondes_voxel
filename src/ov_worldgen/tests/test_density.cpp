// The density function graph, loaded from the real vanilla data.
//
// The values cannot be checked here — the oracle for those is the terrain the
// game itself writes for the same seed. What is checked is that the *whole
// vanilla router loads*, which is a stronger statement than it sounds: it means
// every node type the overworld uses is implemented, every reference resolves,
// every noise is found, and nothing in the graph is circular. A missing node
// type is refused loudly rather than treated as zero, so this test failing is
// how an unimplemented term is discovered instead of shipping terrain that
// looks plausible.

#include "ov/worldgen/density.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

}  // namespace

TEST_CASE("the overworld router loads whole", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent; run tools/ov_datagen first");
    }

    auto router = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(router.has_value());

    // The world's shape, straight from the settings rather than assumed.
    CHECK(router->sea_level() == 63);
    CHECK(router->min_y() == -64);
    CHECK(router->height() == 384);
    CHECK(router->cell_width() == 4);
    CHECK(router->cell_height() == 8);

    // The six climate functions are the ones the biome source reads. Every one
    // of them has to be present, or a biome would be chosen from a missing
    // dimension.
    for (const auto* name : {"temperature", "vegetation", "continents", "erosion", "depth",
                             "ridges", "initial_density_without_jaggedness", "barrier", "lava",
                             "vein_toggle", "vein_ridged", "vein_gap"}) {
        CAPTURE(name);
        CHECK(router->entry(name) != nullptr);
    }

    // And exactly one is not: final_density reaches old_blended_noise, the
    // 1.17 terrain noise, which is not implemented yet. It is named here so
    // that the day it is, this line fails and reminds someone to delete it —
    // rather than the absence quietly becoming permanent.
    const auto missing = router->unavailable();
    REQUIRE(missing.size() == 1);
    CHECK(missing.front().first == "final_density");
    CHECK(missing.front().second == DensityError::Unsupported);
    CHECK(router->entry("final_density") == nullptr);
}

TEST_CASE("climate varies with position and repeats with the seed",
          "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }

    auto first = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(first.has_value());
    const DensityFunction* continents = first->entry("continents");
    REQUIRE(continents != nullptr);

    // Deterministic.
    CHECK(continents->compute({100, 0, 200}) == continents->compute({100, 0, 200}));

    // Not constant: a climate that did not vary would put one biome everywhere,
    // and the failure would look like a working generator.
    bool varies = false;
    const f64 reference = continents->compute({0, 0, 0});
    for (i32 i = 1; i <= 40 && !varies; ++i) {
        varies = continents->compute({i * 512, 0, i * 512}) != reference;
    }
    CHECK(varies);

    // The same seed gives the same world.
    auto again = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(again.has_value());
    CHECK(again->entry("continents")->compute({777, 0, -333}) ==
          continents->compute({777, 0, -333}));

    // A different seed does not.
    auto other = NoiseRouter::load(data_root(), "overworld", 987654321);
    REQUIRE(other.has_value());
    CHECK(other->entry("continents")->compute({777, 0, -333}) !=
          continents->compute({777, 0, -333}));
}

TEST_CASE("flat_cache quantises to quarters", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", 42);
    REQUIRE(router.has_value());
    const DensityFunction* continents = router->entry("continents");
    REQUIRE(continents != nullptr);

    // Continentalness goes through a flat_cache, so it is constant inside each
    // four-by-four column and steps between them. Treating that cache as an
    // identity gives a smooth field where the game has a stepped one, and every
    // biome boundary moves — so this is worth asserting rather than assuming.
    const f64 base = continents->compute({16, 0, 16});
    CHECK(continents->compute({17, 0, 16}) == base);
    CHECK(continents->compute({16, 0, 19}) == base);
    CHECK(continents->compute({19, 0, 19}) == base);
    CHECK(continents->compute({20, 0, 16}) != base);

    // And negative coordinates floor rather than truncate, or the world would
    // be asymmetric about the origin. -4 and -1 land in the same cell — an
    // arithmetic shift of -1 by two is -1, not 0 — while -5 lands in the one
    // below. Getting this wrong makes the world's negative half a cell out of
    // step with its positive half.
    const f64 negative = continents->compute({-4, 0, -4});
    CHECK(continents->compute({-1, 0, -1}) == negative);
    CHECK(continents->compute({-3, 0, -3}) == negative);
    CHECK(continents->compute({-5, 0, -5}) != negative);
}
