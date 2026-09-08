#include "ov/render/terrain_vertex.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

TEST_CASE("the vertex is eight bytes and every field has its own bits", "[vertex]") {
    STATIC_REQUIRE(sizeof(TerrainVertex) == 8);

    // Changing any one field must change the packed word and nothing else.
    // Overlapping bit ranges are the classic packing bug and they show up as
    // lighting that flickers when a texture coordinate changes.
    TerrainVertexAttributes attributes;
    attributes.position    = {1.0F, 2.0F, 3.0F};
    attributes.u           = 4.0F;
    attributes.v           = 5.0F;
    attributes.sky_light   = 11;
    attributes.block_light = 7;
    attributes.ao          = 2;
    attributes.facing      = Direction::North;
    attributes.tint        = TintChannel::Foliage;

    const auto packed   = pack_vertex(attributes);
    const auto unpacked = unpack_vertex(packed);

    CHECK(unpacked.position.x == Approx(1.0F));
    CHECK(unpacked.position.y == Approx(2.0F));
    CHECK(unpacked.position.z == Approx(3.0F));
    CHECK(unpacked.u == Approx(4.0F));
    CHECK(unpacked.v == Approx(5.0F));
    CHECK(unpacked.sky_light == 11);
    CHECK(unpacked.block_light == 7);
    CHECK(unpacked.ao == 2);
    CHECK(unpacked.facing == Direction::North);
    CHECK(unpacked.shade);
    CHECK(unpacked.tint == TintChannel::Foliage);
}

TEST_CASE("the two commonest texture coordinates are exact", "[vertex]") {
    // Every full-face quad in the game carries u or v of 0 and 16. A packing
    // that could only get near them would shrink every block face by a
    // fraction of a texel, which is exactly the kind of error that is invisible
    // in a screenshot and obvious as a seam.
    TerrainVertexAttributes attributes;
    attributes.u = 16.0F;
    attributes.v = 0.0F;

    auto unpacked = unpack_vertex(pack_vertex(attributes));
    CHECK(unpacked.u == 16.0F);
    CHECK(unpacked.v == 0.0F);

    attributes.u = 8.5F;
    unpacked     = unpack_vertex(pack_vertex(attributes));
    CHECK(unpacked.u == 8.5F);
}

TEST_CASE("position keeps its quantisation step", "[vertex]") {
    // 1/64 of a block is the documented precision. A value on the grid has to
    // survive exactly; one between grid points snaps to the nearer step.
    TerrainVertexAttributes attributes;
    attributes.position = {0.015625F, 0.0F, 15.984375F};

    const auto unpacked = unpack_vertex(pack_vertex(attributes));
    CHECK(unpacked.position.x == Approx(0.015625F));
    CHECK(unpacked.position.z == Approx(15.984375F));
}

TEST_CASE("an element that overhangs its block still fits", "[vertex]") {
    // The model format lets `from`/`to` run from -16 to 32 sixteenths, so a
    // vertex can sit a whole block outside the section it belongs to. The range
    // is wider than that on purpose.
    TerrainVertexAttributes attributes;
    attributes.position = {-1.0F, 17.0F, -1.0F};

    const auto unpacked = unpack_vertex(pack_vertex(attributes));
    CHECK(unpacked.position.x == Approx(-1.0F));
    CHECK(unpacked.position.y == Approx(17.0F));
}

TEST_CASE("out-of-range values clamp instead of wrapping", "[vertex]") {
    // Wrapping would put a vertex on the far side of the section, which reads
    // as a stray triangle across the world and is very hard to trace back.
    TerrainVertexAttributes attributes;
    attributes.position    = {-1000.0F, 1000.0F, 0.0F};
    attributes.u           = 999.0F;
    attributes.sky_light   = 200;
    attributes.block_light = 200;
    attributes.ao          = 200;

    const auto unpacked = unpack_vertex(pack_vertex(attributes));
    CHECK(unpacked.position.x == Approx(kPositionMin));
    CHECK(unpacked.position.y == Approx(kPositionMax));
    CHECK(unpacked.u == Approx(kTexCoordMax));
    CHECK(unpacked.sky_light == 15);
    CHECK(unpacked.block_light == 15);
    CHECK(unpacked.ao == 3);
}

TEST_CASE("an unshaded face gives up its direction rather than a bit", "[vertex]") {
    // `"shade": false` elements — the cross models, the dripleaf — take no
    // directional shading, so their facing is not needed and the spare code of
    // the three-bit field carries the flag instead.
    TerrainVertexAttributes attributes;
    attributes.facing = Direction::West;
    attributes.shade  = false;

    const auto unpacked = unpack_vertex(pack_vertex(attributes));
    CHECK_FALSE(unpacked.shade);
}

TEST_CASE("distinct attributes give distinct words", "[vertex]") {
    const TerrainVertexAttributes base;

    auto with_light      = base;
    with_light.sky_light = 3;
    auto with_ao         = base;
    with_ao.ao           = 0;
    auto with_tint       = base;
    with_tint.tint       = TintChannel::Water;

    CHECK_FALSE(pack_vertex(base) == pack_vertex(with_light));
    CHECK_FALSE(pack_vertex(base) == pack_vertex(with_ao));
    CHECK_FALSE(pack_vertex(base) == pack_vertex(with_tint));
}
