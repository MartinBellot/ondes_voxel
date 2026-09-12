// ── temples ── The scattered pieces' start: facing, box and storage, against
// starts the real 1.20.1 server stored.

#include "../src/scattered.hpp"
#include "ov/worldgen/structure_nbt.hpp"
#include "ov/worldgen/structure_set.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] StructurePiece generated(ScatteredKind kind, i64 seed, i32 chunk_x, i32 chunk_z) {
    math::LegacyRandomSource random{large_feature_seed(seed, chunk_x, chunk_z)};
    return make_scattered_piece(kind, chunk_x, chunk_z, random);
}

}  // namespace

TEST_CASE("a scattered piece faces the first draw and stands at its chunk's corner",
          "[structures][temples]") {
    // struct-locate-1234567890's swamp hut, chunk (231, 76): O 3 (east), turned,
    // BB [3696 64 1216, 3704 70 1222].
    const auto hut = generated(ScatteredKind::SwampHut, 1234567890, 231, 76);
    CHECK(hut.scattered.orientation == 3);
    CHECK(hut.box.min_x == 3696);
    CHECK(hut.box.min_y == 64);
    CHECK(hut.box.min_z == 1216);
    CHECK(hut.box.max_x == 3704);
    CHECK(hut.box.max_y == 70);
    CHECK(hut.box.max_z == 1222);
    CHECK_FALSE(hut.height_settled);

    // Its jungle temple of chunk (35, 115): O 0 (south), BB [560 64 1840, 571 73 1854].
    const auto temple = generated(ScatteredKind::JungleTemple, 1234567890, 35, 115);
    CHECK(temple.scattered.orientation == 0);
    CHECK(temple.box.max_x == 571);
    CHECK(temple.box.max_y == 73);
    CHECK(temple.box.max_z == 1854);

    // reference-987654321's swamp hut, chunk (1862, -1873): O 1 (west), turned,
    // BB [29792 64 -29968, 29800 70 -29962].
    const auto west = generated(ScatteredKind::SwampHut, 987654321, 1862, -1873);
    CHECK(west.scattered.orientation == 1);
    CHECK(west.box.max_x == 29800);
    CHECK(west.box.max_z == -29962);

    // struct-locate-1234567890's desert pyramid, chunk (906, 1634): O 2, 21 by 21.
    const auto pyramid = generated(ScatteredKind::DesertPyramid, 1234567890, 906, 1634);
    CHECK(pyramid.scattered.orientation == 2);
    CHECK(pyramid.box.max_x == 14516);
    CHECK(pyramid.box.max_y == 78);
    CHECK(pyramid.box.max_z == 26164);
}

TEST_CASE("a scattered piece writes the game's fields and reads them back",
          "[structures][temples]") {
    auto hut            = generated(ScatteredKind::SwampHut, 1234567890, 231, 76);
    hut.scattered.hpos  = 64;
    hut.scattered.witch = true;
    const nbt::Tag tag  = piece_to_nbt(hut);
    CHECK(tag.find("id")->as_string() == "minecraft:tesh");
    CHECK(tag.find("O")->as_i64() == 3);
    CHECK(tag.find("Width")->as_i64() == 7);
    CHECK(tag.find("Depth")->as_i64() == 9);
    CHECK(tag.find("HPos")->as_i64() == 64);
    CHECK(tag.find("Witch")->as_bool());
    CHECK_FALSE(tag.find("Cat")->as_bool());
    CHECK(tag.find("Template") == nullptr);

    StructurePiece back;
    back.kind           = PieceKind::Scattered;
    back.scattered.kind = *scattered_kind_of(tag.find("id")->as_string());
    back.box            = hut.box;
    REQUIRE(scattered_from_nbt(tag, back));
    CHECK(back.scattered.orientation == 3);
    CHECK(back.scattered.hpos == 64);
    CHECK(back.scattered.witch);
    CHECK(back.height_settled);

    auto pyramid                       = generated(ScatteredKind::DesertPyramid, 1234567890, 906, 1634);
    pyramid.scattered.placed_chests[2] = true;
    const nbt::Tag stored              = piece_to_nbt(pyramid);
    CHECK(stored.find("id")->as_string() == "minecraft:tedp");
    CHECK(stored.find("hasPlacedChest2")->as_bool());
    CHECK_FALSE(stored.find("hasPlacedChest0")->as_bool());
}
