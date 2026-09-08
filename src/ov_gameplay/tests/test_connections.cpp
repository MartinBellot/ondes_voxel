#include "ov/gameplay/connections.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<Connections>             rules;
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
            out.rules.emplace(*out.blocks, *out.registries);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] registry::BlockId block_of(std::string_view name) {
    const auto id = loaded().blocks->find_block(name);
    REQUIRE(id.has_value());
    return *id;
}

[[nodiscard]] registry::BlockStateId state_of(std::string_view name) {
    return loaded().blocks->default_state(block_of(name));
}

[[nodiscard]] registry::BlockStateId with(std::string_view name, std::string_view property,
                                          std::string_view value) {
    const auto block = block_of(name);
    const auto found = loaded().blocks->find_property(block, property);
    REQUIRE(found.has_value());
    const auto it = std::ranges::find(found->values, value);
    REQUIRE(it != found->values.end());
    return loaded().blocks->with_property(
        loaded().blocks->first_state(block), *found,
        static_cast<u16>(std::distance(found->values.begin(), it)));
}

[[nodiscard]] bool fence_reaches(Side side, registry::BlockStateId neighbour) {
    return loaded().rules->attaches(loaded().rules->kind_of(block_of("minecraft:oak_fence")), side,
                                    neighbour);
}

}  // namespace

TEST_CASE("a fence reaches for a full face and for its own kind", "[gameplay][connections]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    REQUIRE(fence_reaches(Side::East, state_of("minecraft:stone")));
    REQUIRE(fence_reaches(Side::East, state_of("minecraft:glass")));
    REQUIRE(fence_reaches(Side::East, state_of("minecraft:spruce_fence")));
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:air")));
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:torch")));

    // Wood keeps to wood: the nether brick fence is its own family, and it is
    // the single case the shape alone gets wrong.
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:nether_brick_fence")));

    // Measured refusals. A full cube, and still no.
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:oak_leaves")));
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:pumpkin")));
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:shulker_box")));
    // The one nobody would have guessed.
    REQUIRE_FALSE(fence_reaches(Side::East, state_of("minecraft:target")));

    // And the two whose support face is full although their box is short.
    REQUIRE(fence_reaches(Side::East, state_of("minecraft:soul_sand")));
    REQUIRE(fence_reaches(Side::East, state_of("minecraft:mud")));
}

TEST_CASE("the same block answers differently by state", "[gameplay][connections]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    // A bottom slab fills the floor and no side; doubled, it fills everything.
    // This is the whole reason the table is per state.
    REQUIRE_FALSE(fence_reaches(Side::East, with("minecraft:oak_slab", "type", "bottom")));
    REQUIRE_FALSE(fence_reaches(Side::East, with("minecraft:oak_slab", "type", "top")));
    REQUIRE(fence_reaches(Side::East, with("minecraft:oak_slab", "type", "double")));
}

TEST_CASE("a gate is reached for across its axis only", "[gameplay][connections]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    // A gate facing north lines up along east and west.
    const auto north = with("minecraft:oak_fence_gate", "facing", "north");
    REQUIRE(fence_reaches(Side::East, north));
    REQUIRE(fence_reaches(Side::West, north));
    REQUIRE_FALSE(fence_reaches(Side::North, north));
    REQUIRE_FALSE(fence_reaches(Side::South, north));

    const auto east = with("minecraft:oak_fence_gate", "facing", "east");
    REQUIRE(fence_reaches(Side::North, east));
    REQUIRE_FALSE(fence_reaches(Side::East, east));
}

TEST_CASE("reshaping sets the four sides at once", "[gameplay][connections]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    const auto air   = state_of("minecraft:air");
    const auto stone = state_of("minecraft:stone");
    const auto fence = state_of("minecraft:oak_fence");

    const auto alone = loaded().rules->reshape(fence, {air, air, air, air});
    for (const std::string_view side : kSideNames) {
        const auto property = loaded().blocks->find_property(block_of("minecraft:oak_fence"), side);
        REQUIRE(property.has_value());
        REQUIRE(loaded().blocks->property_value(alone, *property) == "false");
    }

    // North and east only, in the order the sides are named.
    const auto reached = loaded().rules->reshape(fence, {stone, air, air, stone});
    const auto north   = loaded().blocks->find_property(block_of("minecraft:oak_fence"), "north");
    const auto south   = loaded().blocks->find_property(block_of("minecraft:oak_fence"), "south");
    const auto east    = loaded().blocks->find_property(block_of("minecraft:oak_fence"), "east");
    REQUIRE(loaded().blocks->property_value(reached, *north) == "true");
    REQUIRE(loaded().blocks->property_value(reached, *south) == "false");
    REQUIRE(loaded().blocks->property_value(reached, *east) == "true");

    // Anything that does not reshape comes back untouched.
    REQUIRE(loaded().rules->reshape(stone, {stone, stone, stone, stone}) == stone);
}
