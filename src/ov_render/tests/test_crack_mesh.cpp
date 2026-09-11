#include "ov/render/crack_mesh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

namespace {

[[nodiscard]] BakedQuad quad(Direction facing, std::array<Vec3f, 4> corners) {
    BakedQuad out;
    out.facing = facing;
    for (usize i = 0; i < 4; ++i) {
        out.vertices[i].position = corners[i];
        // The block's own sprite coordinates, which the crack must ignore.
        out.vertices[i].u = 3.0F;
        out.vertices[i].v = 7.0F;
    }
    return out;
}

}  // namespace

TEST_CASE("a full face takes the whole crack texture", "[crack]") {
    // The top of a full block: corners at y = 1 across the whole square.
    BakedModel model;
    model.quads.push_back(quad(Direction::Up, {Vec3f{0, 1, 0}, Vec3f{0, 1, 1}, Vec3f{1, 1, 1},
                                               Vec3f{1, 1, 0}}));
    std::vector<EntityVertex> out;
    build_crack_quads(model, BlockPos{10, 64, -3}, out);
    REQUIRE(out.size() == 4);
    CHECK(out[0].x == 10.0F);
    CHECK(out[0].y == 65.0F);
    CHECK(out[0].z == -3.0F);
    CHECK(out[2].x == 11.0F);
    CHECK(out[2].z == -2.0F);
    f32 u_min = 1.0F;
    f32 u_max = 0.0F;
    f32 v_min = 1.0F;
    f32 v_max = 0.0F;
    for (const EntityVertex& v : out) {
        u_min = std::min(u_min, v.u);
        u_max = std::max(u_max, v.u);
        v_min = std::min(v_min, v.v);
        v_max = std::max(v_max, v.v);
        CHECK(v.colour == std::array<u8, 4>{255, 255, 255, 255});
    }
    CHECK(u_min == 0.0F);
    CHECK(u_max == 1.0F);
    CHECK(v_min == 0.0F);
    CHECK(v_max == 1.0F);
}

TEST_CASE("a slab cracks on its half, cut where the slab ends", "[crack]") {
    // A bottom slab's north side: y from 0 to 0.5.
    BakedModel model;
    model.quads.push_back(quad(Direction::North,
                               {Vec3f{1, 0, 0}, Vec3f{0, 0, 0}, Vec3f{0, 0.5F, 0},
                                Vec3f{1, 0.5F, 0}}));
    std::vector<EntityVertex> out;
    build_crack_quads(model, BlockPos{0, 0, 0}, out);
    REQUIRE(out.size() == 4);
    // The lower half of the texture: v from 0.5 (the slab's top edge) to 1
    // (the ground), exactly as that half of a full block's side.
    CHECK(out[0].v == Approx(1.0F));
    CHECK(out[2].v == Approx(0.5F));
    // Seen from the north the right-hand edge is at x = 0.
    CHECK(out[1].u == Approx(1.0F));
    CHECK(out[0].u == Approx(0.0F));
}

TEST_CASE("the projection is upright on every side", "[crack]") {
    // The top of each side face is v = 0.
    for (const Direction side : {Direction::North, Direction::South, Direction::West,
                                 Direction::East}) {
        CHECK(crack_uv(side, Vec3f{0.3F, 1.0F, 0.6F})[1] == Approx(0.0F));
        CHECK(crack_uv(side, Vec3f{0.3F, 0.0F, 0.6F})[1] == Approx(1.0F));
    }
    // u grows to the right as seen from outside: east of a south face, south
    // of an east face... that is, clockwise round the block seen from above.
    CHECK(crack_uv(Direction::South, Vec3f{0.2F, 0.5F, 1.0F})[0] ==
          Approx(0.2F));
    CHECK(crack_uv(Direction::North, Vec3f{0.2F, 0.5F, 0.0F})[0] == Approx(0.8F));
    CHECK(crack_uv(Direction::West, Vec3f{0.0F, 0.5F, 0.2F})[0] == Approx(0.2F));
    CHECK(crack_uv(Direction::East, Vec3f{1.0F, 0.5F, 0.2F})[0] == Approx(0.8F));
    // An element overhanging its block keeps counting, for the repeating sampler.
    CHECK(crack_uv(Direction::Up, Vec3f{1.25F, 1.0F, -0.5F})[0] == Approx(1.25F));
}
