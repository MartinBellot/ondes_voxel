// ── nether-2 ── What the Nether's features share that a test can see.
//
// The nine feature types themselves are built from the datapack through
// `FeatureRegistry` (src/nether_feature.cpp); this header only exposes the one
// piece of geometry two of them walk in a fixed order.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"

namespace ov::worldgen {

/// `BlockPos.withinManhattan(centre, rx, ry, rz)`: every position within the
/// three reaches, in rings of growing Manhattan distance, x from low to high,
/// then y, and each z != 0 followed at once by its mirror in z.
///
/// The order is part of the seed: the delta and the blobs stop at the first
/// position past their reach, and a later position overwrites an earlier one.
class ManhattanWalk {
public:
    ManhattanWalk(BlockPos centre, i32 rx, i32 ry, i32 rz) noexcept
        : centre_(centre), rx_(rx), ry_(ry), rz_(rz), max_depth_(rx + ry + rz) {}

    /// The next position, or false once every ring is done.
    [[nodiscard]] bool next(BlockPos& out) noexcept;

private:
    BlockPos centre_;
    BlockPos cursor_{};
    i32      rx_;
    i32      ry_;
    i32      rz_;
    i32      max_depth_;
    i32      depth_{0};
    i32      max_x_{0};
    i32      max_y_{0};
    i32      x_{0};
    i32      y_{0};
    bool     z_mirror_{false};
};

}  // namespace ov::worldgen
