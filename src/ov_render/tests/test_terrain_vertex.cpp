#include "ov/render/terrain_vertex.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

TEST_CASE("the vertex is sixteen bytes and every field has its own bits", "[vertex]") {
    STATIC_REQUIRE(sizeof(TerrainVertex) == 16);

    // Overlapping bit ranges are the classic packing bug, and they show up as
    // lighting that flickers when a texture coordinate changes.
    TerrainVertexAttributes attributes;
    attributes.position    = {1.0F, 2.0F, 3.0F};
    attributes.u           = 0.25F;
    attributes.v           = 0.75F;
    attributes.sky_quarters   = 43;
    attributes.block_quarters = 29;
    attributes.occlusion      = 0.6F;
    attributes.facing      = Direction::North;
    attributes.tint_colour = 0x91BD59;

    const auto unpacked = unpack_vertex(pack_vertex(attributes));

    CHECK(unpacked.position.x == Approx(1.0F));
    CHECK(unpacked.position.y == Approx(2.0F));
    CHECK(unpacked.position.z == Approx(3.0F));
    CHECK(unpacked.u == Approx(0.25F).margin(1e-4));
    CHECK(unpacked.v == Approx(0.75F).margin(1e-4));
    CHECK(unpacked.sky_quarters == 43);
    CHECK(unpacked.block_quarters == 29);
    // Eight bits: the four corner values 1.0, 0.8, 0.6, 0.4 are within half a
    // step of 1/255.
    CHECK(unpacked.occlusion == Approx(0.6F).margin(0.5 / 255.0));
    CHECK(unpacked.facing == Direction::North);
    CHECK(unpacked.shade);
    // Eight bits a channel, so a real biome colour round-trips exactly.
    CHECK(unpacked.tint_colour == 0x91BD59);
}

TEST_CASE("texture coordinates address a texel of a real atlas", "[vertex]") {
    // This is the check the eight-byte layout failed, and the reason the format
    // is twelve bytes. Eight bits of u put every vertex on a four-texel grid of
    // a 1024 atlas, so every block sampled the wrong part of its own texture —
    // and a round-trip test that only asked "did the value come back" said
    // nothing about it.
    constexpr u32 kAtlasSize = 1024;

    for (u32 texel : {0u, 1u, 17u, 511u, 512u, 1023u}) {
        TerrainVertexAttributes attributes;
        attributes.u = static_cast<f32>(texel) / static_cast<f32>(kAtlasSize);

        const auto unpacked = unpack_vertex(pack_vertex(attributes));
        const auto recovered =
            static_cast<u32>(std::lround(unpacked.u * static_cast<f32>(kAtlasSize)));
        CHECK(recovered == texel);
    }
}

TEST_CASE("every coordinate a model can state survives exactly", "[vertex]") {
    // Model coordinates are sixteenths of a block, and the scale is a power of
    // two, so none of them is ever rounded.
    for (i32 sixteenth = -16; sixteenth <= 32; ++sixteenth) {
        TerrainVertexAttributes attributes;
        attributes.position.x = static_cast<f32>(sixteenth) / 16.0F;

        const auto unpacked = unpack_vertex(pack_vertex(attributes));
        CHECK(unpacked.position.x == static_cast<f32>(sixteenth) / 16.0F);
    }
}

TEST_CASE("an element that overhangs its block still fits", "[vertex]") {
    // The model format lets `from`/`to` run from -16 to 32 sixteenths, so a
    // vertex can sit a whole block outside the section it belongs to.
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
    attributes.u              = 9.0F;
    attributes.sky_quarters   = 200;
    attributes.block_quarters = 200;
    attributes.occlusion      = 7.0F;

    const auto unpacked = unpack_vertex(pack_vertex(attributes));
    CHECK(unpacked.position.x == Approx(kPositionMin));
    CHECK(unpacked.position.y == Approx(kPositionMax));
    CHECK(unpacked.u == Approx(1.0F));
    CHECK(unpacked.sky_quarters == kMaxLightQuarters);
    CHECK(unpacked.block_quarters == kMaxLightQuarters);
    CHECK(unpacked.occlusion == Approx(1.0F));
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

    auto with_light         = base;
    with_light.sky_quarters = 3;
    auto with_ao            = base;
    with_ao.occlusion       = 0.4F;
    auto with_tint       = base;
    with_tint.tint_colour = 0x3F76E4;

    CHECK_FALSE(pack_vertex(base) == pack_vertex(with_light));
    CHECK_FALSE(pack_vertex(base) == pack_vertex(with_ao));
    CHECK_FALSE(pack_vertex(base) == pack_vertex(with_tint));
}
