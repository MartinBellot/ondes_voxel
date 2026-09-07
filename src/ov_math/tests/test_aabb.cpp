#include "ov/math/aabb.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using Catch::Approx;

namespace {
/// A player is 0.6 wide and 1.8 tall; the position is between the feet.
constexpr AABB player_at(f64 x, f64 y, f64 z) {
    return AABB::from_entity(Vec3d{x, y, z}, 0.6, 1.8);
}
}  // namespace

TEST_CASE("an entity box is centred horizontally and rests on its position", "[math][aabb]") {
    // Getting the anchor wrong buries the player half a block into the floor.
    const AABB box = player_at(0.0, 64.0, 0.0);
    REQUIRE(box.min.x == Approx(-0.3));
    REQUIRE(box.max.x == Approx(0.3));
    REQUIRE(box.min.y == Approx(64.0));
    REQUIRE(box.max.y == Approx(65.8));
    REQUIRE(box.size().y == Approx(1.8));
}

TEST_CASE("touching faces do not count as overlap", "[math][aabb]") {
    // This is what lets a player stand on a block instead of sinking into it.
    const AABB block{Vec3d{0, 63, 0}, Vec3d{1, 64, 1}};
    const AABB standing = player_at(0.5, 64.0, 0.5);

    REQUIRE_FALSE(standing.intersects(block));
    REQUIRE(standing.translated(Vec3d{0, -0.001, 0}).intersects(block));
}

TEST_CASE("the swept box covers the whole path", "[math][aabb]") {
    const AABB box{Vec3d{0, 0, 0}, Vec3d{1, 1, 1}};

    const AABB forward = box.swept(Vec3d{2, 0, 0});
    REQUIRE(forward.min.x == Approx(0.0));
    REQUIRE(forward.max.x == Approx(3.0));

    const AABB backward = box.swept(Vec3d{-2, 0, 0});
    REQUIRE(backward.min.x == Approx(-2.0));
    REQUIRE(backward.max.x == Approx(1.0));
}

TEST_CASE("movement is clipped by a blocking box", "[math][aabb]") {
    const AABB wall{Vec3d{1, 0, 0}, Vec3d{2, 1, 1}};
    const AABB mover{Vec3d{0, 0, 0}, Vec3d{0.5, 1, 1}};

    // Moving into the wall stops flush against it.
    REQUIRE(mover.clip_x(wall, 2.0) == Approx(0.5));
    // Not far enough to reach it: unchanged.
    REQUIRE(mover.clip_x(wall, 0.25) == Approx(0.25));
    // Moving away: unchanged.
    REQUIRE(mover.clip_x(wall, -1.0) == Approx(-1.0));
}

TEST_CASE("a box misaligned on another axis does not block", "[math][aabb]") {
    // The check that makes sliding along a wall work: a box the mover passes
    // beside must not stop it.
    const AABB above{Vec3d{1, 5, 0}, Vec3d{2, 6, 1}};
    const AABB mover{Vec3d{0, 0, 0}, Vec3d{0.5, 1, 1}};
    REQUIRE(mover.clip_x(above, 2.0) == Approx(2.0));

    const AABB aside{Vec3d{1, 0, 5}, Vec3d{2, 1, 6}};
    REQUIRE(mover.clip_x(aside, 2.0) == Approx(2.0));
}

TEST_CASE("falling is clipped by the floor", "[math][aabb]") {
    const AABB floor{Vec3d{0, 63, 0}, Vec3d{1, 64, 1}};
    const AABB falling = player_at(0.5, 64.5, 0.5);

    // Half a block above the floor, falling a whole block: stops at 0.5.
    REQUIRE(falling.clip_y(floor, -1.0) == Approx(-0.5));
    REQUIRE(falling.clip_y(floor, -0.25) == Approx(-0.25));
    // Rising is unaffected by what is below.
    REQUIRE(falling.clip_y(floor, 1.0) == Approx(1.0));
}

TEST_CASE("a ceiling clips upward movement", "[math][aabb]") {
    const AABB ceiling{Vec3d{0, 66, 0}, Vec3d{1, 67, 1}};
    const AABB player = player_at(0.5, 64.0, 0.5);
    // The box top is at 65.8, so it can rise 0.2 before hitting 66.
    REQUIRE(player.clip_y(ceiling, 1.0) == Approx(0.2));
}

TEST_CASE("inflate and translate", "[math][aabb]") {
    const AABB box{Vec3d{0, 0, 0}, Vec3d{1, 1, 1}};

    const AABB grown = box.inflated(0.5);
    REQUIRE(grown.min.x == Approx(-0.5));
    REQUIRE(grown.max.x == Approx(1.5));

    const AABB moved = box.translated(Vec3d{10, 20, 30});
    REQUIRE(moved.min == Vec3d{10, 20, 30});
    REQUIRE(moved.max == Vec3d{11, 21, 31});
}

TEST_CASE("contains uses a half-open interval", "[math][aabb]") {
    const AABB box{Vec3d{0, 0, 0}, Vec3d{1, 1, 1}};
    REQUIRE(box.contains(Vec3d{0.0, 0.0, 0.0}));
    REQUIRE(box.contains(Vec3d{0.999, 0.999, 0.999}));
    // The far face belongs to the next block, not this one.
    REQUIRE_FALSE(box.contains(Vec3d{1.0, 0.5, 0.5}));
}

TEST_CASE("an empty box is recognised", "[math][aabb]") {
    REQUIRE(AABB{}.is_empty());
    REQUIRE(AABB{Vec3d{1, 1, 1}, Vec3d{1, 2, 2}}.is_empty());
    REQUIRE_FALSE(AABB{Vec3d{0, 0, 0}, Vec3d{1, 1, 1}}.is_empty());
}

TEST_CASE("block bounds handle exact boundaries", "[math][aabb]") {
    // A box from y=0 to y=1 touches block 0 only. Including block 1 makes an
    // entity collide with the block above the one it stands on.
    REQUIRE(aabb_min_block(0.0) == 0);
    REQUIRE(aabb_max_block(1.0) == 0);
    REQUIRE(aabb_max_block(1.5) == 1);
    REQUIRE(aabb_min_block(-0.5) == -1);
    REQUIRE(aabb_min_block(-1.0) == -1);
    REQUIRE(aabb_max_block(-1.0) == -2);
}
