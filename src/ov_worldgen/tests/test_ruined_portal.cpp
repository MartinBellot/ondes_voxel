// ── portals ── The ruined portal's height rule and cold test, on a world
// simple enough to know the answer: solid below `ground`, water below `sea`,
// air above.

#include "../src/ruined_portal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>

using namespace ov;
using namespace ov::worldgen;

namespace {

class FlatSampler final : public StructureWorldSampler {
public:
    i32                ground{70};
    i32                sea{63};
    /// The surface the heightmap reports, when it is not the ground's.
    std::optional<i32> reported;
    std::optional<f32> temperature{0.8F};
    bool               columns{true};

    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:plains";
    }
    [[nodiscard]] i32 surface_height(i32, i32) const override {
        return reported.value_or(std::max(ground, sea));
    }
    [[nodiscard]] i32 ocean_floor_height(i32, i32) const override {
        return reported.value_or(ground);
    }
    [[nodiscard]] std::optional<Substance> base_substance(i32, i32 y, i32) const override {
        if (!columns) {
            return std::nullopt;
        }
        if (y < ground) {
            return Substance::Solid;
        }
        return y < sea ? Substance::Water : Substance::Air;
    }
    [[nodiscard]] std::optional<f32> temperature_at(i32, i32, i32) const override {
        return temperature;
    }
};

/// portal_1's size, placed at y 0: nine tall.
constexpr BoundingBox kBox{0, 0, 0, 7, 8, 6};
constexpr i64         kSeed = 42;

[[nodiscard]] bool untouched(math::LegacyRandomSource& random) {
    math::LegacyRandomSource fresh{kSeed};
    return random.next_long() == fresh.next_long();
}

}  // namespace

TEST_CASE("a portal on the surface stands on the first occupied block, drawing nothing",
          "[structures][portals]") {
    FlatSampler              world;
    math::LegacyRandomSource random{kSeed};
    const auto y = ruined_portal_height(world, "on_land_surface", false, kBox, -64, random);
    REQUIRE(y);
    CHECK(*y == 69);
    CHECK(untouched(random));
}

TEST_CASE("a portal on the sea bed reads the floor, not the water", "[structures][portals]") {
    FlatSampler world;
    world.ground = 40;
    math::LegacyRandomSource random{kSeed};
    const auto y = ruined_portal_height(world, "on_ocean_floor", false, kBox, -64, random);
    REQUIRE(y);
    CHECK(*y == 39);
}

TEST_CASE("an underground portal draws from 15 above the bottom to its height under the surface",
          "[structures][portals]") {
    FlatSampler              world;
    math::LegacyRandomSource random{kSeed};
    const auto y = ruined_portal_height(world, "underground", true, kBox, -64, random);
    REQUIRE(y);
    math::LegacyRandomSource fresh{kSeed};
    CHECK(*y == fresh.next_int(60 - -49 + 1) + -49);

    // Under a surface too low for the interval, the top of it and no draw.
    world.ground = -40;
    world.sea    = -64;
    math::LegacyRandomSource low{kSeed};
    const auto               shallow = ruined_portal_height(world, "underground", true, kBox, -64, low);
    REQUIRE(shallow);
    CHECK(*shallow == -41 - 9);
    CHECK(untouched(low));
}

TEST_CASE("a partly buried portal rises 2 to 8 above its buried height", "[structures][portals]") {
    FlatSampler              world;
    math::LegacyRandomSource random{kSeed};
    const auto y = ruined_portal_height(world, "partly_buried", false, kBox, -64, random);
    REQUIRE(y);
    math::LegacyRandomSource fresh{kSeed};
    CHECK(*y == 69 - 9 + fresh.next_int(7) + 2);
}

TEST_CASE("a portal settles down until three corners stand on something",
          "[structures][portals]") {
    FlatSampler world;
    world.ground   = 50;
    world.sea      = -64;
    world.reported = 70;
    math::LegacyRandomSource random{kSeed};
    const auto y = ruined_portal_height(world, "on_land_surface", false, kBox, -64, random);
    REQUIRE(y);
    CHECK(*y == 49);

    // Water holds a land portal; only stone holds one on the sea bed.
    world.sea = 63;
    math::LegacyRandomSource again{kSeed};
    CHECK(ruined_portal_height(world, "on_land_surface", false, kBox, -64, again).value() == 62);
    math::LegacyRandomSource sea{kSeed};
    CHECK(ruined_portal_height(world, "on_ocean_floor", false, kBox, -64, sea).value() == 49);

    // Nothing to stand on: the floor, 15 above the bottom.
    world.ground = -100;
    world.sea    = -100;
    math::LegacyRandomSource none{kSeed};
    CHECK(ruined_portal_height(world, "on_land_surface", false, kBox, -64, none).value() == -49);
}

TEST_CASE("a Nether portal with an air pocket draws 32 to 100", "[structures][portals]") {
    FlatSampler world;
    world.ground = 128;
    math::LegacyRandomSource random{kSeed};
    const auto y = ruined_portal_height(world, "in_nether", true, kBox, 0, random);
    REQUIRE(y);
    math::LegacyRandomSource fresh{kSeed};
    CHECK(*y == fresh.next_int(69) + 32);
}

TEST_CASE("a portal is cold below 0.15, and says so when it cannot tell",
          "[structures][portals]") {
    FlatSampler    world;
    const BlockPos origin{0, 69, 0};
    world.temperature = 0.1F;
    CHECK(ruined_portal_cold(world, origin).value());
    world.temperature = 0.15F;
    CHECK_FALSE(ruined_portal_cold(world, origin).value());
    world.temperature.reset();
    CHECK_FALSE(ruined_portal_cold(world, origin).has_value());

    world.columns = false;
    math::LegacyRandomSource random{kSeed};
    CHECK_FALSE(ruined_portal_height(world, "on_land_surface", false, kBox, -64, random));
}
