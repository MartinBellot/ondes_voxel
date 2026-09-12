#pragma once

// ── treasure ── The buried treasure's downward search.
//
// The start says nothing of the height (it stores y 90 until it is placed); the
// chest's y is found at placement, in the real blocks. The wiki gives only the
// column (chunk-relative 9, 9) and the facing. The search was read off the
// game's output: from the first free block of the sea-bed column, down to the
// first position whose block below is a support block; the chest stands there.
// 13 finished treasures of four worlds and three seeds, 13 of 13; without
// sandstone in the support set, 4 of 13 (docs/provenance/structures.md, § 15.2).

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/placement.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ov::worldgen {

/// The blocks a buried treasure's chest rests on, by name. Sandstone and
/// granite are the ones the reference worlds exercise; the other stones of the
/// same ground are kept with them.
inline constexpr std::array<std::string_view, 5> kTreasureSupport{
    "minecraft:sandstone", "minecraft:stone", "minecraft:granite", "minecraft:diorite",
    "minecraft:andesite"};

/// The chest's y in the column (`x`, `z`): from the ocean-floor heightmap's
/// first free block, down to the first y whose block below is in `support`.
/// The world's bottom when nothing below qualifies.
[[nodiscard]] i32 buried_treasure_height(const FeatureLevel&                    level,
                                         std::span<const registry::BlockStateId> support, i32 x,
                                         i32 z);

}  // namespace ov::worldgen
