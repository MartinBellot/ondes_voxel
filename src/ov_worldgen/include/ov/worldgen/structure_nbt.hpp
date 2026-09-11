// A chunk's `structures` compound, written the way 1.20.1 writes it.
//
// Two lists live there. `starts` holds, by structure name, every structure
// whose *start chunk* this is — the start's chunk coordinates and its pieces,
// each piece in its own type's NBT. `References` holds, by structure name, the
// packed positions of the start chunks of every structure whose box crosses
// this chunk. The game reads both back: the first to place the pieces it has
// not placed yet, the second to find a structure from any chunk it covers
// (`/locate`, a mob's "am I inside a fortress", a treasure map).
//
// The field names and tag types are the game's, read on the chunks of the
// reference worlds (`scripts`-side dump, docs/provenance/structures.md § 20):
// booleans are bytes, the integrity and the mossiness floats, the box an int
// array of six, `GD` and `O` ints. A template piece stores `TPX/TPY/TPZ`, its
// `Template` and `Rot` — the ruined portal alone spells it `Rotation` and adds
// `Mirror`.
//
// The inverse of `StructureBuilder::piece_from_nbt`, and tested as such.
#pragma once

#include "ov/base/types.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <span>
#include <string>

namespace ov::worldgen {

/// `ChunkPos.asLong`: x in the low 32 bits, z in the high 32.
[[nodiscard]] constexpr i64 packed_chunk_pos(i32 chunk_x, i32 chunk_z) noexcept {
    return static_cast<i64>(static_cast<u64>(static_cast<u32>(chunk_x)) |
                            (static_cast<u64>(static_cast<u32>(chunk_z)) << 32));
}

/// One piece, as the game stores it in `Children`.
[[nodiscard]] nbt::Tag piece_to_nbt(const StructurePiece& piece);

/// One start: `id`, `ChunkX`, `ChunkZ`, `references`, `Children`.
[[nodiscard]] nbt::Tag start_to_nbt(const StructureStart& start);

/// A structure crossing a chunk, named by its start chunk.
struct StructureReference {
    std::string structure;
    i32         chunk_x{0};
    i32         chunk_z{0};
};

/// The whole `structures` compound: `starts` from the starts given (a start
/// without pieces is skipped — the game stores only valid starts) and
/// `References`, one long array per structure, duplicates dropped.
[[nodiscard]] nbt::Tag chunk_structures_to_nbt(std::span<const StructureStart>     starts,
                                               std::span<const StructureReference> references);

}  // namespace ov::worldgen
