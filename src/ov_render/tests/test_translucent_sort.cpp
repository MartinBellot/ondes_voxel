#include "ov/render/translucent_sort.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

namespace {

/// A unit quad in the y = height plane, at (x, z).
void add_quad(std::vector<TerrainVertex>& out, f32 x, f32 height, f32 z) {
    for (const auto [dx, dz] : {std::pair{0.0F, 0.0F}, std::pair{1.0F, 0.0F}, std::pair{1.0F, 1.0F},
                                std::pair{0.0F, 1.0F}}) {
        TerrainVertexAttributes attributes;
        attributes.position = Vec3f{x + dx, height, z + dz};
        out.push_back(pack_vertex(attributes));
    }
}

}  // namespace

TEST_CASE("a quad's centre is the mean of its four corners", "[sort]") {
    std::vector<TerrainVertex> vertices;
    add_quad(vertices, 3.0F, 5.0F, 7.0F);
    add_quad(vertices, 0.0F, 15.0F, 0.0F);

    std::vector<Vec3f> centres;
    quad_centres(vertices, centres);
    REQUIRE(centres.size() == 2);
    CHECK(centres[0].x == Approx(3.5F));
    CHECK(centres[0].y == Approx(5.0F));
    CHECK(centres[0].z == Approx(7.5F));
    CHECK(centres[1].y == Approx(15.0F));
}

TEST_CASE("quads are ordered farthest first, and the order follows the eye", "[sort]") {
    // A column of water surfaces at heights 1, 5 and 9: from above the lowest
    // is drawn first, from below the highest.
    const std::vector<Vec3f> centres{{0.5F, 5.0F, 0.5F}, {0.5F, 1.0F, 0.5F}, {0.5F, 9.0F, 0.5F}};
    std::vector<u32>         order;

    sort_back_to_front(centres, Vec3f{0.5F, 20.0F, 0.5F}, order);
    CHECK(order == std::vector<u32>{1, 0, 2});

    sort_back_to_front(centres, Vec3f{0.5F, -10.0F, 0.5F}, order);
    CHECK(order == std::vector<u32>{2, 0, 1});
}

TEST_CASE("equal distances keep mesh order", "[sort]") {
    const std::vector<Vec3f> centres{{1.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    std::vector<u32>         order;
    sort_back_to_front(centres, Vec3f{0.0F, 0.0F, 0.0F}, order);
    CHECK(order == std::vector<u32>{0, 1, 2});
}

TEST_CASE("the index list is the shared pattern, quad by quad in the order given", "[sort]") {
    std::vector<u32> indices;
    write_quad_indices(std::vector<u32>{2, 0}, indices);
    CHECK(indices == std::vector<u32>{8, 9, 10, 8, 10, 11, 0, 1, 2, 0, 2, 3});
}
