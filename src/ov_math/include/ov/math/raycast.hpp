// Walking a ray through a voxel grid.
//
// This is what happens every frame when the player looks at a block: which
// block is under the crosshair, and which face of it. It is also how vanilla
// picks the block a right-click applies to, so the face matters as much as the
// position — placing a torch depends on it.
//
// The algorithm is Amanatides and Woo's grid traversal: step from one cell to
// the next along whichever axis has the nearest boundary, never sampling a cell
// the ray does not actually enter. That last part is why a naive "step by a
// small amount and round" loop is wrong rather than merely slow: it skips cells
// at shallow angles, and the player mines the block behind the one they aimed
// at.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"

#include <cmath>
#include <limits>
#include <optional>

namespace ov {

/// Floor to a block coordinate. Named apart from the AABB helper so that
/// including one header does not silently depend on the other.
[[nodiscard]] constexpr i32 floor_to_block(f64 value) noexcept {
    const auto floored = static_cast<i32>(value);
    return value < static_cast<f64>(floored) ? floored - 1 : floored;
}

struct RayHit {
    /// The block the ray entered.
    BlockPos block{};
    /// The face it entered through — the side a placed block would attach to.
    Direction face{Direction::Up};
    /// Distance from the origin, in blocks.
    f64 distance{0.0};
    /// The exact point of entry.
    Vec3d position{};
};

/// Walk a ray through the voxel grid, calling `is_solid(BlockPos)` for each cell
/// it enters, and stop at the first that says yes.
///
/// `max_distance` is in blocks; vanilla's reach is 4.5 in survival and 5 in
/// creative. The origin's own block is tested first, since a player standing
/// inside a block should hit it immediately.
template<typename SolidPredicate>
[[nodiscard]] std::optional<RayHit> raycast_voxels(const Vec3d& origin, const Vec3d& direction,
                                                   f64 max_distance, SolidPredicate is_solid) {
    // A zero-length direction has no ray to walk; returning nothing beats
    // dividing by zero and walking to infinity.
    const f64 length = direction.length();
    if (length <= 0.0 || max_distance <= 0.0) {
        return std::nullopt;
    }
    const Vec3d dir = direction * (1.0 / length);

    i32 x = floor_to_block(origin.x);
    i32 y = floor_to_block(origin.y);
    i32 z = floor_to_block(origin.z);

    if (is_solid(BlockPos{x, y, z})) {
        return RayHit{BlockPos{x, y, z}, Direction::Up, 0.0, origin};
    }

    // Which way each axis steps, and how far along the ray one whole cell is.
    const i32 step_x = dir.x > 0.0 ? 1 : (dir.x < 0.0 ? -1 : 0);
    const i32 step_y = dir.y > 0.0 ? 1 : (dir.y < 0.0 ? -1 : 0);
    const i32 step_z = dir.z > 0.0 ? 1 : (dir.z < 0.0 ? -1 : 0);

    // Infinity for an axis the ray does not move along: that axis then never
    // wins the "nearest boundary" comparison, which is exactly right.
    constexpr f64 kInfinity = std::numeric_limits<f64>::infinity();

    const f64 delta_x = step_x != 0 ? std::abs(1.0 / dir.x) : kInfinity;
    const f64 delta_y = step_y != 0 ? std::abs(1.0 / dir.y) : kInfinity;
    const f64 delta_z = step_z != 0 ? std::abs(1.0 / dir.z) : kInfinity;

    // Distance along the ray to the first boundary on each axis.
    auto first_boundary = [](f64 position, i32 cell, i32 step, f64 delta) -> f64 {
        if (step == 0) {
            return kInfinity;
        }
        const f64 fraction = position - static_cast<f64>(cell);
        return (step > 0 ? (1.0 - fraction) : fraction) * delta;
    };

    f64 next_x = first_boundary(origin.x, x, step_x, delta_x);
    f64 next_y = first_boundary(origin.y, y, step_y, delta_y);
    f64 next_z = first_boundary(origin.z, z, step_z, delta_z);

    while (true) {
        // Step along whichever axis reaches its boundary first.
        f64       travelled;
        Direction entered_face;

        if (next_x <= next_y && next_x <= next_z) {
            travelled = next_x;
            if (travelled > max_distance)
                return std::nullopt;
            x += step_x;
            next_x += delta_x;
            // Entered through the face opposite the direction of travel.
            entered_face = step_x > 0 ? Direction::West : Direction::East;
        } else if (next_y <= next_z) {
            travelled = next_y;
            if (travelled > max_distance)
                return std::nullopt;
            y += step_y;
            next_y += delta_y;
            entered_face = step_y > 0 ? Direction::Down : Direction::Up;
        } else {
            travelled = next_z;
            if (travelled > max_distance)
                return std::nullopt;
            z += step_z;
            next_z += delta_z;
            entered_face = step_z > 0 ? Direction::North : Direction::South;
        }

        const BlockPos cell{x, y, z};
        if (is_solid(cell)) {
            return RayHit{cell, entered_face, travelled, origin + dir * travelled};
        }
    }
}

}  // namespace ov
