#include "ov/render/block_shape.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace ov;
using namespace ov::render;

TEST_CASE("a cube has twelve edges", "[shape]") {
    std::vector<std::array<Vec3d, 2>> edges;
    shape_edges(std::array{AABB{Vec3d{0, 0, 0}, Vec3d{1, 1, 1}}}, edges);
    CHECK(edges.size() == 12);
    for (const auto& [a, b] : edges) {
        const Vec3d d = b - a;
        CHECK(d.x + d.y + d.z == 1.0);  // each spans the whole side
    }
}

TEST_CASE("two slabs stacked are one cube: no seam", "[shape]") {
    std::vector<std::array<Vec3d, 2>> edges;
    shape_edges(std::array{AABB{Vec3d{0, 0, 0}, Vec3d{1, 0.5, 1}},
                           AABB{Vec3d{0, 0.5, 0}, Vec3d{1, 1, 1}}},
                edges);
    CHECK(edges.size() == 12);
}

TEST_CASE("a stair shows its step", "[shape]") {
    // Bottom half, and the back half of the top: an L-shaped prism along x.
    std::vector<std::array<Vec3d, 2>> edges;
    shape_edges(std::array{AABB{Vec3d{0, 0, 0}, Vec3d{1, 0.5, 1}},
                           AABB{Vec3d{0, 0.5, 0.5}, Vec3d{1, 1, 1}}},
                edges);
    // Six corners of the L on each end (twelve edges) and six along x.
    CHECK(edges.size() == 18);
}

TEST_CASE("a torch-thin box is outlined whole", "[shape]") {
    std::vector<std::array<Vec3d, 2>> edges;
    shape_edges(std::array{AABB{Vec3d{0.4375, 0, 0.4375}, Vec3d{0.5625, 0.625, 0.5625}}}, edges);
    CHECK(edges.size() == 12);
}

TEST_CASE("the bounds of several boxes", "[shape]") {
    const std::array boxes{AABB{Vec3d{0, 0, 0}, Vec3d{1, 0.5, 1}},
                           AABB{Vec3d{0.25, 0.5, 0.5}, Vec3d{0.75, 1, 1}}};
    const AABB b = bounds_of(boxes);
    CHECK(b.min.x == 0.0);
    CHECK(b.max.y == 1.0);
    CHECK(b.min.z == 0.0);
    CHECK(bounds_of(std::span<const AABB>{}).is_empty());
}
