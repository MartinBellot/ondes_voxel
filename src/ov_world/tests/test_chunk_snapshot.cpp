// Chunk snapshots: what the tick thread hands to the network thread.
//
// CLAUDE.md principle 3: the tick thread is the only writer of a published
// chunk, and sharing goes through `shared_ptr<const>` — copy-on-write. A
// snapshot must keep what the chunk held when it was taken, whatever the tick
// writes afterwards, and taking one must not copy 98304 blocks: sections share
// their storage until one side writes.
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_section.hpp"
#include "ov/world/light_array.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace ov;
using namespace ov::world;
using registry::BlockStateId;

namespace {

/// The real air ids, as in test_chunk_section.cpp.
constexpr AirStates kAir{BlockStateId{0}, BlockStateId{12817}, BlockStateId{12818}};

constexpr BlockStateId kStone{1};
constexpr BlockStateId kGranite{2};

[[nodiscard]] Chunk make_chunk() {
    return Chunk{ChunkPos{3, -7}, WorldShape::overworld(), kAir, nullptr};
}

/// The address of a section's packed block data: equal addresses mean the two
/// sections share one storage, which is the whole point of a snapshot.
[[nodiscard]] const u16* palette_address(const ChunkSection& section) {
    return section.blocks().palette().data();
}

[[nodiscard]] usize index_of_y(const Chunk& chunk, i32 y) {
    return static_cast<usize>((y >> 4) - chunk.shape().min_section());
}

}  // namespace

TEST_CASE("a snapshot keeps what the chunk held when it was taken", "[world][snapshot]") {
    Chunk chunk = make_chunk();
    chunk.set_block(4, 64, 9, kStone);

    const std::shared_ptr<const Chunk> snapshot = chunk.snapshot();

    chunk.set_block(4, 64, 9, kGranite);
    chunk.set_block(0, -64, 0, kStone);

    REQUIRE(snapshot->get_block(4, 64, 9) == kStone);
    REQUIRE(snapshot->get_block(0, -64, 0) == kAir.air);
    REQUIRE(snapshot->non_air_count() == 1);

    REQUIRE(chunk.get_block(4, 64, 9) == kGranite);
    REQUIRE(chunk.get_block(0, -64, 0) == kStone);
    REQUIRE(chunk.non_air_count() == 2);

    REQUIRE(snapshot->position().x == 3);
    REQUIRE(snapshot->position().z == -7);
}

TEST_CASE("only a written section stops sharing its storage", "[world][snapshot]") {
    // Taking a snapshot must cost pointers, not blocks: 24 sections of up to
    // 8 KiB each, copied on every chunk sent, is the cost this design exists
    // to avoid.
    Chunk chunk = make_chunk();
    chunk.set_block(1, 10, 1, kStone);
    chunk.set_block(1, 100, 1, kStone);

    const std::shared_ptr<const Chunk> snapshot = chunk.snapshot();

    for (usize i = 0; i < chunk.sections().size(); ++i) {
        REQUIRE(palette_address(chunk.sections()[i]) == palette_address(snapshot->sections()[i]));
    }

    chunk.set_block(2, 10, 2, kGranite);

    const usize written = index_of_y(chunk, 10);
    for (usize i = 0; i < chunk.sections().size(); ++i) {
        const bool shared =
            palette_address(chunk.sections()[i]) == palette_address(snapshot->sections()[i]);
        CAPTURE(i);
        REQUIRE(shared == (i != written));
    }
}

TEST_CASE("light written after a snapshot is not the snapshot's", "[world][snapshot]") {
    // Light is written through a reference (`block_light()`), not through a
    // setter on the section: the reference itself has to be unshared.
    Chunk chunk = make_chunk();
    chunk.section_for_y(0)->block_light() = LightArray{0};
    chunk.section_for_y(0)->block_light().set(section_index(5, 0, 5), 9);

    const std::shared_ptr<const Chunk> snapshot = chunk.snapshot();

    chunk.section_for_y(0)->block_light().set(section_index(5, 0, 5), 14);
    chunk.section_for_y(0)->sky_light() = LightArray{15};

    REQUIRE(snapshot->section_for_y(0)->block_light().get(section_index(5, 0, 5)) == 9);
    REQUIRE(snapshot->section_for_y(0)->sky_light().is_absent());
    REQUIRE(chunk.section_for_y(0)->block_light().get(section_index(5, 0, 5)) == 14);
    REQUIRE(chunk.section_for_y(0)->sky_light().get(0) == 15);
}

TEST_CASE("a plain copy is independent in both directions", "[world][snapshot]") {
    // Sharing storage is an implementation detail: a Chunk still behaves as a
    // value, whichever side writes first.
    Chunk original = make_chunk();
    original.set_block(7, 20, 7, kStone);

    Chunk copy = original;
    copy.set_block(7, 20, 7, kGranite);
    REQUIRE(original.get_block(7, 20, 7) == kStone);
    REQUIRE(copy.get_block(7, 20, 7) == kGranite);

    Chunk second = original;
    original.set_block(7, 20, 7, kAir.air);
    REQUIRE(second.get_block(7, 20, 7) == kStone);
    REQUIRE(original.get_block(7, 20, 7) == kAir.air);
    REQUIRE(original.non_air_count() == 0);
    REQUIRE(second.non_air_count() == 1);
}

TEST_CASE("a reference from a writable section outlives the snapshot", "[world][snapshot]") {
    // The shape of the relight pass: take a writable section, write its light,
    // keep reading its blocks through a reference. If unsharing happened on
    // the light write rather than when the section was handed out, that
    // reference would point into storage only the snapshot owns — and the
    // snapshot is released by another thread whenever it likes.
    Chunk chunk = make_chunk();
    chunk.set_block(3, 5, 3, kStone);

    std::shared_ptr<const Chunk> snapshot = chunk.snapshot();

    ChunkSection*            section = chunk.section_for_y(5);
    const PalettedContainer& blocks  = section->blocks();
    section->block_light()           = LightArray{0};

    snapshot.reset();  // what the network thread does once the bytes are out

    section->block_light().set(section_index(3, 5, 3), 7);
    REQUIRE(blocks.get(section_index(3, 5, 3)) == kStone.value());
    REQUIRE(section->block_light().get(section_index(3, 5, 3)) == 7);
}

TEST_CASE("snapshots of snapshots stay put", "[world][snapshot]") {
    // Two sends of the same chunk in flight at once: a player joining while
    // another is already being sent it.
    Chunk chunk = make_chunk();
    chunk.set_block(0, 0, 0, kStone);
    const auto first = chunk.snapshot();
    chunk.set_block(0, 0, 0, kGranite);
    const auto second = chunk.snapshot();
    chunk.set_block(0, 0, 0, kAir.air);

    REQUIRE(first->get_block(0, 0, 0) == kStone);
    REQUIRE(second->get_block(0, 0, 0) == kGranite);
    REQUIRE(chunk.get_block(0, 0, 0) == kAir.air);
}
