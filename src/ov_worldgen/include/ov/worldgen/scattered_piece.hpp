#pragma once

// ── temples ── The code-built scattered pieces: the swamp hut, the desert
// pyramid and the jungle temple. The game builds them in code, not from
// templates; their layouts are reconstructed black-box from the blocks the real
// 1.20.1 server writes (docs/provenance/structures.md, § temples). This header
// is the per-piece state only, so that `StructurePiece` can carry it.

#include "ov/base/types.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace ov::worldgen {

enum class ScatteredKind : u8 {
    SwampHut,
    DesertPyramid,
    JungleTemple,
};

/// The per-piece state the game stores with a scattered piece (`tesh`, `tedp`,
/// `tejp`), field for field.
struct ScatteredPiece {
    ScatteredKind kind{ScatteredKind::SwampHut};
    /// The piece's facing, as its 2D data value: south 0, west 1, north 2,
    /// east 3 (`O`).
    i32 orientation{0};
    /// The piece's own size, before its facing turns it (`Width`, `Height`,
    /// `Depth`).
    i32 width{0};
    i32 height{0};
    i32 depth{0};
    /// The settled ground height; -1 until the piece is first placed (`HPos`).
    i32 hpos{-1};
    /// Swamp hut: the witch and the cat have been spawned (`Witch`, `Cat`).
    bool witch{false};
    bool cat{false};
    /// Desert pyramid: its four chests have been placed (`hasPlacedChest0..3`).
    std::array<bool, 4> placed_chests{};
    /// Jungle temple: `placedMainChest`, `placedHiddenChest`, `placedTrap1`,
    /// `placedTrap2`.
    bool placed_main_chest{false};
    bool placed_hidden_chest{false};
    bool placed_trap1{false};
    bool placed_trap2{false};
};

/// `minecraft:tesh`, `minecraft:tedp`, `minecraft:tejp`.
[[nodiscard]] std::string_view scattered_piece_id(ScatteredKind kind) noexcept;

/// The kind a stored piece id names, or nothing.
[[nodiscard]] std::optional<ScatteredKind> scattered_kind_of(std::string_view id) noexcept;

}  // namespace ov::worldgen
