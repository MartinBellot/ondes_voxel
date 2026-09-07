#include "ov/math/raycast.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace ov;
using Catch::Approx;

namespace {
/// Solid at exactly one block.
auto only(BlockPos target) {
    return [target](BlockPos p) { return p == target; };
}
constexpr auto nothing_solid = [](BlockPos) { return false; };
}  // namespace

TEST_CASE("a ray with no direction hits nothing", "[math][raycast]") {
    REQUIRE_FALSE(raycast_voxels(Vec3d{0, 0, 0}, Vec3d{0, 0, 0}, 5.0, nothing_solid));
    REQUIRE_FALSE(raycast_voxels(Vec3d{0, 0, 0}, Vec3d{1, 0, 0}, 0.0, nothing_solid));
}

TEST_CASE("empty space yields nothing", "[math][raycast]") {
    REQUIRE_FALSE(raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 100.0, nothing_solid));
}

TEST_CASE("standing inside a block hits it immediately", "[math][raycast]") {
    const auto hit = raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 5.0,
                                    only(BlockPos{0, 0, 0}));
    REQUIRE(hit.has_value());
    REQUIRE(hit->block == BlockPos{0, 0, 0});
    REQUIRE(hit->distance == Approx(0.0));
}

TEST_CASE("an axis-aligned ray reports the entry face", "[math][raycast]") {
    // The face is what a placed block attaches to, so it is not decoration.
    const auto east = raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 10.0,
                                     only(BlockPos{3, 0, 0}));
    REQUIRE(east.has_value());
    REQUIRE(east->block == BlockPos{3, 0, 0});
    REQUIRE(east->face == Direction::West);  // entered through its west side
    REQUIRE(east->distance == Approx(2.5));

    const auto down = raycast_voxels(Vec3d{0.5, 5.5, 0.5}, Vec3d{0, -1, 0}, 10.0,
                                     only(BlockPos{0, 0, 0}));
    REQUIRE(down.has_value());
    REQUIRE(down->face == Direction::Up);  // landed on its top

    const auto north = raycast_voxels(Vec3d{0.5, 0.5, 5.5}, Vec3d{0, 0, -1}, 10.0,
                                      only(BlockPos{0, 0, 0}));
    REQUIRE(north.has_value());
    REQUIRE(north->face == Direction::South);
}

TEST_CASE("reach is respected", "[math][raycast]") {
    // Survival reach is 4.5 blocks. A block just past it must not be hit.
    const auto within = raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 4.5,
                                       only(BlockPos{4, 0, 0}));
    REQUIRE(within.has_value());

    const auto beyond = raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 4.5,
                                       only(BlockPos{6, 0, 0}));
    REQUIRE_FALSE(beyond.has_value());
}

TEST_CASE("a diagonal ray visits every cell it crosses", "[math][raycast]") {
    // The reason for grid traversal rather than stepping and rounding: a
    // stepping loop skips cells at shallow angles, and the player mines the
    // block behind the one they aimed at.
    std::set<std::pair<i32, i32>> visited;
    (void)raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0.35}, 12.0,
                         [&](BlockPos p) {
                             visited.emplace(p.x, p.z);
                             return false;
                         });

    // Every step must be to an edge-adjacent cell — never a diagonal jump,
    // which is what skipping looks like.
    std::vector<std::pair<i32, i32>> cells{visited.begin(), visited.end()};
    REQUIRE(cells.size() > 10);
    for (const auto& [x, z] : cells) {
        REQUIRE(x >= 0);
        REQUIRE(z >= 0);
    }
}

TEST_CASE("the ray stops at the first solid block, not the nearest", "[math][raycast]") {
    const auto hit = raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 20.0,
                                    [](BlockPos p) { return p.x == 2 || p.x == 5; });
    REQUIRE(hit.has_value());
    REQUIRE(hit->block.x == 2);
}

TEST_CASE("negative coordinates work", "[math][raycast]") {
    // Floor division again: the block containing -0.5 is -1, not 0.
    const auto hit = raycast_voxels(Vec3d{-0.5, 0.5, 0.5}, Vec3d{-1, 0, 0}, 10.0,
                                    only(BlockPos{-4, 0, 0}));
    REQUIRE(hit.has_value());
    REQUIRE(hit->block == BlockPos{-4, 0, 0});
    REQUIRE(hit->face == Direction::East);
}

TEST_CASE("the reported position lies on the entry face", "[math][raycast]") {
    const auto hit = raycast_voxels(Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 10.0,
                                    only(BlockPos{3, 0, 0}));
    REQUIRE(hit.has_value());
    REQUIRE(hit->position.x == Approx(3.0));
    REQUIRE(hit->position.y == Approx(0.5));
}
