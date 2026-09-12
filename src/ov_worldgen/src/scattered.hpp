#pragma once

// ── temples ── The scattered pieces' generation and storage.
//
// What the start decides, measured on 22 stored starts of three seeds
// (reference-1234567890, struct-locate-1234567890, reference-987654321 and the
// out-of-sample 20260911), 22 of 22: the piece faces the first draw after the
// large-feature seed, `nextInt(4)` over north, east, south, west; its box stands
// at the chunk's corner at y 64 until it is placed, as wide as its facing turns
// it. The other draw orders give 0, 6 and 0 of 22.

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/worldgen/scattered_piece.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_pieces.hpp"

#include <expected>
#include <optional>
#include <string>

namespace ov::worldgen {

/// A piece's own size, before its facing turns it.
struct ScatteredSize {
    i32 width{0};
    i32 height{0};
    i32 depth{0};
};

/// Where a scattered piece's box stands until it is first placed.
inline constexpr i32 kScatteredPlaceholderY = 64;

[[nodiscard]] ScatteredSize scattered_size(ScatteredKind kind) noexcept;

/// The scattered piece a structure type builds, or nothing.
[[nodiscard]] std::optional<ScatteredKind> scattered_kind_for(StructureKind kind) noexcept;

/// The start's one piece. Draws the facing from `random`, and nothing else.
[[nodiscard]] StructurePiece make_scattered_piece(ScatteredKind kind, i32 chunk_x, i32 chunk_z,
                                                  math::LegacyRandomSource& random);

/// The piece's own fields, after the `id`, `BB`, `GD` and `O` every piece has.
void scattered_to_nbt(const StructurePiece& piece, nbt::Tag& out);

/// Read the piece's own fields (and `O`) into `piece`, whose kind and box are set.
[[nodiscard]] std::expected<void, std::string> scattered_from_nbt(const nbt::Tag& child,
                                                                  StructurePiece& piece);

}  // namespace ov::worldgen
