// Axis-aligned bounding boxes, and the swept collision they support.
//
// Minecraft's entity physics is entirely AABB against AABB: no rotation, no
// convex hulls. A player is a box 0.6 wide and 1.8 tall, and moving it means
// asking how far it can travel along each axis before something stops it.
//
// The detail that matters, and that a naive implementation gets wrong: movement
// is resolved one axis at a time, in the order Y, X, Z. Testing the full
// displacement at once makes a player walking into a wall stop dead instead of
// sliding along it, and makes a step up a block impossible. Vanilla resolves
// per axis, and any difference here shows up as a player who cannot climb
// stairs.
#pragma once

#include "ov/math/vec.hpp"

#include <algorithm>

namespace ov {

/// A box in world space. `min` is inclusive, `max` exclusive in spirit: a box
/// touching another exactly at a face does not overlap it, which is what lets a
/// player stand on a block rather than sink into it.
struct AABB {
    Vec3d min{};
    Vec3d max{};

    constexpr AABB() noexcept = default;

    constexpr AABB(const Vec3d& min_corner, const Vec3d& max_corner) noexcept
        : min{min_corner}, max{max_corner} {}

    /// A box of the given size, centred horizontally on `position` and resting
    /// on it vertically. This is how Minecraft anchors an entity: the position
    /// is the point between the feet, not the centre of the box.
    [[nodiscard]] static constexpr AABB from_entity(const Vec3d& position, f64 width,
                                                    f64 height) noexcept {
        const f64 half = width / 2.0;
        return AABB{Vec3d{position.x - half, position.y, position.z - half},
                    Vec3d{position.x + half, position.y + height, position.z + half}};
    }

    [[nodiscard]] constexpr Vec3d size() const noexcept { return max - min; }

    [[nodiscard]] constexpr Vec3d centre() const noexcept {
        return Vec3d{(min.x + max.x) * 0.5, (min.y + max.y) * 0.5, (min.z + max.z) * 0.5};
    }

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return max.x <= min.x || max.y <= min.y || max.z <= min.z;
    }

    [[nodiscard]] constexpr AABB translated(const Vec3d& delta) const noexcept {
        return AABB{min + delta, max + delta};
    }

    /// Grow by `amount` on every side. Negative shrinks.
    [[nodiscard]] constexpr AABB inflated(f64 amount) const noexcept {
        return AABB{Vec3d{min.x - amount, min.y - amount, min.z - amount},
                    Vec3d{max.x + amount, max.y + amount, max.z + amount}};
    }

    /// The box covering everywhere this one passes through while moving by
    /// `delta`. Used to decide which blocks are worth testing at all.
    [[nodiscard]] constexpr AABB swept(const Vec3d& delta) const noexcept {
        return AABB{
            Vec3d{delta.x < 0 ? min.x + delta.x : min.x, delta.y < 0 ? min.y + delta.y : min.y,
                  delta.z < 0 ? min.z + delta.z : min.z},
            Vec3d{delta.x > 0 ? max.x + delta.x : max.x, delta.y > 0 ? max.y + delta.y : max.y,
                  delta.z > 0 ? max.z + delta.z : max.z}};
    }

    /// True when the two boxes share volume. Touching faces do not count.
    [[nodiscard]] constexpr bool intersects(const AABB& other) const noexcept {
        return min.x < other.max.x && max.x > other.min.x && min.y < other.max.y &&
               max.y > other.min.y && min.z < other.max.z && max.z > other.min.z;
    }

    [[nodiscard]] constexpr bool contains(const Vec3d& point) const noexcept {
        return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y &&
               point.z >= min.z && point.z < max.z;
    }

    /// How far this box may move along X before `other` stops it.
    ///
    /// Returns `delta` unchanged when the two do not overlap on the other two
    /// axes, or when the movement takes it away. The Y and Z forms are the same
    /// with the axes rotated; they are written out rather than templated
    /// because this is the hottest code in entity movement and the explicit
    /// version is what a profiler can read.
    [[nodiscard]] constexpr f64 clip_x(const AABB& other, f64 delta) const noexcept {
        if (max.y <= other.min.y || min.y >= other.max.y)
            return delta;
        if (max.z <= other.min.z || min.z >= other.max.z)
            return delta;

        // Moving forward: the obstacle is ahead when its near face is at or
        // past this box's far face, and the gap between them is how far this
        // box may travel.
        if (delta > 0.0 && other.min.x >= max.x) {
            return std::min(delta, other.min.x - max.x);
        }
        if (delta < 0.0 && other.max.x <= min.x) {
            return std::max(delta, other.max.x - min.x);
        }
        return delta;
    }

    [[nodiscard]] constexpr f64 clip_y(const AABB& other, f64 delta) const noexcept {
        if (max.x <= other.min.x || min.x >= other.max.x)
            return delta;
        if (max.z <= other.min.z || min.z >= other.max.z)
            return delta;

        // Moving forward: the obstacle is ahead when its near face is at or
        // past this box's far face, and the gap between them is how far this
        // box may travel.
        if (delta > 0.0 && other.min.y >= max.y) {
            return std::min(delta, other.min.y - max.y);
        }
        if (delta < 0.0 && other.max.y <= min.y) {
            return std::max(delta, other.max.y - min.y);
        }
        return delta;
    }

    [[nodiscard]] constexpr f64 clip_z(const AABB& other, f64 delta) const noexcept {
        if (max.x <= other.min.x || min.x >= other.max.x)
            return delta;
        if (max.y <= other.min.y || min.y >= other.max.y)
            return delta;

        // Moving forward: the obstacle is ahead when its near face is at or
        // past this box's far face, and the gap between them is how far this
        // box may travel.
        if (delta > 0.0 && other.min.z >= max.z) {
            return std::min(delta, other.min.z - max.z);
        }
        if (delta < 0.0 && other.max.z <= min.z) {
            return std::max(delta, other.max.z - min.z);
        }
        return delta;
    }

    friend constexpr bool operator==(const AABB&, const AABB&) noexcept = default;
};

/// The block the box occupies, for iterating candidate colliders.
///
/// The upper bound is exclusive when the box ends exactly on a boundary: a box
/// from y=0 to y=1 touches only block y=0, not y=1. Getting this wrong makes an
/// entity collide with the block above the one it stands on.
[[nodiscard]] constexpr i32 aabb_min_block(f64 value) noexcept {
    const auto floored = static_cast<i32>(value);
    return value < static_cast<f64>(floored) ? floored - 1 : floored;
}

[[nodiscard]] constexpr i32 aabb_max_block(f64 value) noexcept {
    const i32 floored = aabb_min_block(value);
    return value == static_cast<f64>(floored) ? floored - 1 : floored;
}

}  // namespace ov
