#include "ov/gameplay/collision.hpp"

namespace ov::gameplay {
namespace {

/// The shapes are stored in thirty-seconds of a block.
constexpr f64 kUnit = 1.0 / 32.0;

}  // namespace

void CollisionWorld::boxes_at(i32 x, i32 y, i32 z, std::vector<AABB>& out) const {
    const registry::BlockStateId state = lookup_(context_, x, y, z);
    for (const registry::BlockRegistry::Box& box : blocks_->collision_boxes(state)) {
        out.push_back(AABB{Vec3d{static_cast<f64>(x) + static_cast<f64>(box.min_x) * kUnit,
                                 static_cast<f64>(y) + static_cast<f64>(box.min_y) * kUnit,
                                 static_cast<f64>(z) + static_cast<f64>(box.min_z) * kUnit},
                           Vec3d{static_cast<f64>(x) + static_cast<f64>(box.max_x) * kUnit,
                                 static_cast<f64>(y) + static_cast<f64>(box.max_y) * kUnit,
                                 static_cast<f64>(z) + static_cast<f64>(box.max_z) * kUnit}});
    }
}

bool CollisionWorld::overlaps(const AABB& box) const {
    std::vector<AABB> shapes;
    for (i32 y = aabb_min_block(box.min.y); y <= aabb_max_block(box.max.y); ++y) {
        for (i32 z = aabb_min_block(box.min.z); z <= aabb_max_block(box.max.z); ++z) {
            for (i32 x = aabb_min_block(box.min.x); x <= aabb_max_block(box.max.x); ++x) {
                shapes.clear();
                boxes_at(x, y, z, shapes);
                for (const AABB& shape : shapes) {
                    if (shape.intersects(box)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

Vec3d CollisionWorld::slide(const AABB& box, const Vec3d& delta) const {
    // Gather once, over the whole swept volume: a box moving a block and a half
    // touches cells its start and end positions do not.
    const AABB        reach = box.swept(delta).inflated(1.0);
    std::vector<AABB> shapes;
    for (i32 y = aabb_min_block(reach.min.y); y <= aabb_max_block(reach.max.y); ++y) {
        for (i32 z = aabb_min_block(reach.min.z); z <= aabb_max_block(reach.max.z); ++z) {
            for (i32 x = aabb_min_block(reach.min.x); x <= aabb_max_block(reach.max.x); ++x) {
                boxes_at(x, y, z, shapes);
            }
        }
    }

    // Y first, then X, then Z. The order is vanilla's, and it is what lets a
    // player slide along a wall rather than stop against it.
    Vec3d allowed{delta.x, delta.y, delta.z};
    AABB  moving = box;

    for (const AABB& shape : shapes) {
        allowed.y = moving.clip_y(shape, allowed.y);
    }
    moving = moving.translated(Vec3d{0.0, allowed.y, 0.0});

    for (const AABB& shape : shapes) {
        allowed.x = moving.clip_x(shape, allowed.x);
    }
    moving = moving.translated(Vec3d{allowed.x, 0.0, 0.0});

    for (const AABB& shape : shapes) {
        allowed.z = moving.clip_z(shape, allowed.z);
    }
    return allowed;
}

}  // namespace ov::gameplay
