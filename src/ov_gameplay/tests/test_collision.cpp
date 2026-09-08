#include "ov/gameplay/collision.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        Loaded     out;
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        if (auto blocks = registry::BlockRegistry::load(path)) {
            out.blocks = std::move(*blocks);
        }
        return out;
    }();
    return state;
}

/// A world of a handful of blocks, addressed by position. Air everywhere else.
struct TinyWorld {
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> blocks;

    static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
        const auto* self = static_cast<const TinyWorld*>(context);
        const auto  it   = self->blocks.find({x, y, z});
        return it == self->blocks.end() ? registry::BlockStateId{0} : it->second;
    }
};

[[nodiscard]] registry::BlockStateId state_of(std::string_view name) {
    const auto block = loaded().blocks->find_block(name);
    REQUIRE(block.has_value());
    return loaded().blocks->default_state(*block);
}

[[nodiscard]] registry::BlockStateId slab(std::string_view type) {
    const auto block = loaded().blocks->find_block("minecraft:oak_slab");
    REQUIRE(block.has_value());
    const auto property = loaded().blocks->find_property(*block, "type");
    REQUIRE(property.has_value());
    for (u16 index = 0; index < property->values.size(); ++index) {
        if (property->values[index] == type) {
            return loaded().blocks->with_property(loaded().blocks->first_state(*block), *property,
                                                  index);
        }
    }
    FAIL("no such slab");
    return registry::BlockStateId{0};
}

}  // namespace

TEST_CASE("a box overlaps the shapes around it", "[gameplay][collision]") {
    if (!loaded().blocks) {
        SKIP("no registry pack");
    }
    TinyWorld world;
    world.blocks[{0, 0, 0}] = state_of("minecraft:stone");
    const CollisionWorld collisions{*loaded().blocks, &TinyWorld::look_up, &world};

    // Standing on top of it: touching a face is not overlapping, which is the
    // whole reason a player rests on a block instead of sinking into it.
    REQUIRE_FALSE(collisions.overlaps(player_box(Vec3d{0.5, 1.0, 0.5})));
    REQUIRE(collisions.overlaps(player_box(Vec3d{0.5, 0.9, 0.5})));

    // Beside it, just clear and just not: the player is 0.6 wide, so the box
    // runs from x - 0.3 to x + 0.3.
    REQUIRE_FALSE(collisions.overlaps(player_box(Vec3d{1.3, 0.0, 0.5})));
    REQUIRE(collisions.overlaps(player_box(Vec3d{1.29, 0.0, 0.5})));

    // And nothing at all where there is no block.
    REQUIRE_FALSE(collisions.overlaps(player_box(Vec3d{8.5, 0.0, 8.5})));
}

TEST_CASE("half a block is half a block", "[gameplay][collision]") {
    if (!loaded().blocks) {
        SKIP("no registry pack");
    }
    TinyWorld world;
    world.blocks[{0, 0, 0}] = slab("bottom");
    world.blocks[{2, 0, 0}] = slab("top");
    const CollisionWorld collisions{*loaded().blocks, &TinyWorld::look_up, &world};

    // A bottom slab stops at half height, so a player stands at 0.5 and not at
    // 1.0. Reading one box per block would put them a whole block up.
    REQUIRE_FALSE(collisions.overlaps(player_box(Vec3d{0.5, 0.5, 0.5})));
    REQUIRE(collisions.overlaps(player_box(Vec3d{0.5, 0.49, 0.5})));

    // A top slab occupies the upper half instead, so the floor is still at 1.0
    // and there is nothing below it.
    REQUIRE(collisions.overlaps(player_box(Vec3d{2.5, 0.5, 0.5})));
    REQUIRE_FALSE(collisions.overlaps(player_box(Vec3d{2.5, 1.0, 0.5})));
}

TEST_CASE("a stair is two boxes, not one", "[gameplay][collision]") {
    if (!loaded().blocks) {
        SKIP("no registry pack");
    }
    TinyWorld world;
    world.blocks[{0, 0, 0}] = state_of("minecraft:oak_stairs");
    const CollisionWorld collisions{*loaded().blocks, &TinyWorld::look_up, &world};

    std::vector<AABB> shapes;
    collisions.boxes_at(0, 0, 0, shapes);
    REQUIRE(shapes.size() >= 2);

    // Its lower half is solid across the block, and its upper half only over
    // part of it — so one corner at 1.0 is clear and the other is not.
    REQUIRE(collisions.overlaps(AABB{Vec3d{0.1, 0.1, 0.1}, Vec3d{0.2, 0.2, 0.2}}));
    const bool upper_front = collisions.overlaps(AABB{Vec3d{0.1, 0.6, 0.1}, Vec3d{0.2, 0.7, 0.2}});
    const bool upper_back  = collisions.overlaps(AABB{Vec3d{0.1, 0.6, 0.8}, Vec3d{0.2, 0.7, 0.9}});
    REQUIRE(upper_front != upper_back);
}

TEST_CASE("moving stops on one axis and keeps the others", "[gameplay][collision]") {
    if (!loaded().blocks) {
        SKIP("no registry pack");
    }
    TinyWorld world;
    // A wall at x = 2, and a floor under the walker.
    for (i32 z = -2; z <= 2; ++z) {
        world.blocks[{2, 0, z}]  = state_of("minecraft:stone");
        world.blocks[{0, -1, z}] = state_of("minecraft:stone");
        world.blocks[{1, -1, z}] = state_of("minecraft:stone");
    }
    const CollisionWorld collisions{*loaded().blocks, &TinyWorld::look_up, &world};

    const AABB standing = player_box(Vec3d{0.5, 0.0, 0.5});

    // Walking into the wall diagonally: the x is cut short, the z is not. This
    // is what sliding along a wall means, and testing the whole displacement at
    // once would stop both.
    const Vec3d allowed = collisions.slide(standing, Vec3d{2.0, 0.0, 1.0});
    REQUIRE(allowed.x < 1.3);
    REQUIRE(allowed.x > 1.1);
    REQUIRE(allowed.z == 1.0);

    // Falling onto the floor stops exactly on it rather than inside.
    const Vec3d dropped = collisions.slide(player_box(Vec3d{0.5, 0.5, 0.5}), Vec3d{0.0, -2.0, 0.0});
    REQUIRE(dropped.y == -0.5);

    // And in open air nothing is cut at all.
    const Vec3d free = collisions.slide(player_box(Vec3d{40.5, 40.0, 40.5}), Vec3d{1.0, -1.0, 1.0});
    REQUIRE(free.x == 1.0);
    REQUIRE(free.y == -1.0);
    REQUIRE(free.z == 1.0);
}
