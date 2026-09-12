#include "ov/gameplay/block_motion.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <vector>

namespace ov::gameplay {
namespace {

struct Entry {
    std::string_view name;
    BlockMotion      motion;
};

[[nodiscard]] BlockMotion slippery(f32 friction) {
    BlockMotion motion;
    motion.friction = friction;
    return motion;
}

[[nodiscard]] BlockMotion slowing(f32 speed, f32 jump = 1.0F) {
    BlockMotion motion;
    motion.speed_factor = speed;
    motion.jump_factor  = jump;
    return motion;
}

[[nodiscard]] BlockMotion holding(Vec3d stuck) {
    BlockMotion motion;
    motion.stuck = stuck;
    return motion;
}

[[nodiscard]] BlockMotion climbing() {
    BlockMotion motion;
    motion.climbable = true;
    return motion;
}

}  // namespace

BlockMotionTable::BlockMotionTable(const registry::BlockRegistry& blocks) : blocks_{&blocks} {
    by_block_.assign(blocks.block_count(), BlockMotion{});

    const auto set = [&](std::string_view name, const BlockMotion& motion) {
        if (const auto block = blocks.find_block(name)) {
            by_block_[block->value()] = motion;
        }
    };

    // Friction. Measured by launching an armor stand and a dropped item across
    // each floor at 0.8 blocks a tick: the ratio of successive velocities on
    // the ground is friction × 0.91 for the stand and friction × 0.98 for the
    // item, and both give the same friction.
    set("minecraft:ice", slippery(0.98F));
    set("minecraft:packed_ice", slippery(0.98F));
    set("minecraft:frosted_ice", slippery(0.98F));
    set("minecraft:blue_ice", slippery(0.989F));

    BlockMotion slime = slippery(0.8F);
    slime.bounces     = true;
    set("minecraft:slime_block", slime);

    BlockMotion honey = slowing(0.4F, 0.5F);
    honey.honey       = true;
    set("minecraft:honey_block", honey);

    BlockMotion soul_sand      = slowing(0.4F);
    soul_sand.soul_speed_block = true;
    set("minecraft:soul_sand", soul_sand);
    BlockMotion soul_soil      = BlockMotion{};
    soul_soil.soul_speed_block = true;
    set("minecraft:soul_soil", soul_soil);

    // Held in place. The move made inside is scaled per axis and the velocity
    // is discarded, so what gravity rebuilds every tick is all that moves it.
    set("minecraft:cobweb", holding(Vec3d{0.25, static_cast<f64>(0.05F), 0.25}));
    BlockMotion berries       = holding(Vec3d{static_cast<f64>(0.8F), 0.75, static_cast<f64>(0.8F)});
    berries.stuck_living_only = true;
    set("minecraft:sweet_berry_bush", berries);
    BlockMotion powder       = holding(Vec3d{static_cast<f64>(0.9F), 1.5, static_cast<f64>(0.9F)});
    powder.stuck_needs_feet  = true;
    set("minecraft:powder_snow", powder);

    // `#minecraft:climbable`, as the 1.20.1 data generator lists it.
    for (const std::string_view name :
         {"minecraft:ladder", "minecraft:vine", "minecraft:weeping_vines",
          "minecraft:weeping_vines_plant", "minecraft:twisting_vines",
          "minecraft:twisting_vines_plant", "minecraft:cave_vines", "minecraft:cave_vines_plant"}) {
        set(name, climbing());
    }
    BlockMotion scaffolding = climbing();
    scaffolding.scaffolding = true;
    set("minecraft:scaffolding", scaffolding);

    BlockMotion water;
    water.is_water = true;
    set("minecraft:water", water);
    BlockMotion column    = water;
    column.bubble_column  = true;
    set("minecraft:bubble_column", column);
    if (const auto block = blocks.find_block("minecraft:bubble_column")) {
        bubble_column_ = *block;
        drag_          = blocks.find_property(*block, "drag");
    }
}

const BlockMotion& BlockMotionTable::of(registry::BlockStateId state) const noexcept {
    const usize block = blocks_->block_of(state).value();
    static constexpr BlockMotion kDefault{};
    return block < by_block_.size() ? by_block_[block] : kDefault;
}

BubbleColumn BlockMotionTable::bubble(registry::BlockStateId state) const noexcept {
    if (!drag_ || blocks_->block_of(state) != bubble_column_) {
        return BubbleColumn::None;
    }
    return blocks_->property_value(state, *drag_) == "true" ? BubbleColumn::Down
                                                            : BubbleColumn::Up;
}

// ── Reading the blocks around a box ────────────────────────────────────────

namespace {

[[nodiscard]] i32 floor_to_int(f64 value) noexcept {
    const auto truncated = static_cast<i32>(value);
    return value < static_cast<f64>(truncated) ? truncated - 1 : truncated;
}

/// How far below the feet the block that moves you is looked for. A float in
/// the game, so the double it widens to is 0.50000101327…, not 0.500001.
constexpr f64 kFloorOffset = static_cast<f64>(0.500001F);

/// The block whose shape the bottom of the box rests on, nearest the centre.
[[nodiscard]] std::optional<Vec3i> supporting_block(const CollisionWorld& world,
                                                    const Vec3d& position, const AABB& box) {
    // The sliver just under the box: whatever it touches is holding it up.
    const AABB sliver{Vec3d{box.min.x, box.min.y - 1.0E-6, box.min.z},
                      Vec3d{box.max.x, box.min.y, box.max.z}};
    std::optional<Vec3i> best;
    f64                  best_distance = 0.0;
    std::vector<AABB>    shapes;
    // One cell further down than the sliver, for shapes taller than a block
    // (a fence's collision is a block and a half).
    for (i32 x = aabb_min_block(sliver.min.x); x <= aabb_max_block(sliver.max.x); ++x) {
        for (i32 y = floor_to_int(sliver.min.y) - 1; y <= floor_to_int(sliver.max.y); ++y) {
            for (i32 z = aabb_min_block(sliver.min.z); z <= aabb_max_block(sliver.max.z); ++z) {
                shapes.clear();
                world.boxes_at(x, y, z, shapes);
                bool touches = false;
                for (const AABB& shape : shapes) {
                    touches = touches || shape.intersects(sliver);
                }
                if (!touches) {
                    continue;
                }
                const f64 dx       = static_cast<f64>(x) + 0.5 - position.x;
                const f64 dy       = static_cast<f64>(y) + 0.5 - position.y;
                const f64 dz       = static_cast<f64>(z) + 0.5 - position.z;
                const f64 distance = dx * dx + dy * dy + dz * dz;
                if (!best || distance < best_distance) {
                    best          = Vec3i{x, y, z};
                    best_distance = distance;
                }
            }
        }
    }
    return best;
}

[[nodiscard]] registry::BlockStateId feet_state(const CollisionWorld& world,
                                                const Vec3d& position) {
    return world.state_at(floor_to_int(position.x), floor_to_int(position.y),
                          floor_to_int(position.z));
}

}  // namespace

registry::BlockStateId movement_floor(const CollisionWorld& world, const Vec3d& position,
                                      const AABB& box, bool on_ground) {
    const i32 y = floor_to_int(position.y - kFloorOffset);
    if (on_ground) {
        if (const auto support = supporting_block(world, position, box)) {
            return world.state_at(support->x, y, support->z);
        }
    }
    return world.state_at(floor_to_int(position.x), y, floor_to_int(position.z));
}

f32 floor_friction(const CollisionWorld& world, const Vec3d& position, const AABB& box,
                   bool on_ground) {
    const BlockMotionTable* table = world.motion();
    if (table == nullptr) {
        return 0.6F;
    }
    return table->of(movement_floor(world, position, box, on_ground)).friction;
}

f32 speed_factor_at(const CollisionWorld& world, const Vec3d& position, const AABB& box,
                    bool on_ground) {
    const BlockMotionTable* table = world.motion();
    if (table == nullptr) {
        return 1.0F;
    }
    const BlockMotion& feet = table->of(feet_state(world, position));
    if (feet.is_water || feet.speed_factor != 1.0F) {
        return feet.speed_factor;
    }
    return table->of(movement_floor(world, position, box, on_ground)).speed_factor;
}

f32 jump_factor_at(const CollisionWorld& world, const Vec3d& position, const AABB& box,
                   bool on_ground) {
    const BlockMotionTable* table = world.motion();
    if (table == nullptr) {
        return 1.0F;
    }
    const f32 feet = table->of(feet_state(world, position)).jump_factor;
    return feet != 1.0F ? feet
                        : table->of(movement_floor(world, position, box, on_ground)).jump_factor;
}

bool on_climbable(const CollisionWorld& world, const Vec3d& position) {
    const BlockMotionTable* table = world.motion();
    if (table == nullptr) {
        return false;
    }
    const registry::BlockStateId feet = feet_state(world, position);
    if (table->of(feet).climbable) {
        return true;
    }
    // An open trapdoor is a ladder rung when a ladder facing the same way is
    // right under it — which is how a ladder climbs through a hatch.
    const registry::BlockRegistry& blocks = world.blocks();
    const registry::BlockId        block  = blocks.block_of(feet);
    if (!blocks.block_name(block).ends_with("_trapdoor")) {
        return false;
    }
    const auto open   = blocks.find_property(block, "open");
    const auto facing = blocks.find_property(block, "facing");
    if (!open || !facing || blocks.property_value(feet, *open) != "true") {
        return false;
    }
    const registry::BlockStateId below =
        world.state_at(floor_to_int(position.x), floor_to_int(position.y) - 1,
                       floor_to_int(position.z));
    const registry::BlockId below_block = blocks.block_of(below);
    if (blocks.block_name(below_block) != "minecraft:ladder") {
        return false;
    }
    const auto ladder_facing = blocks.find_property(below_block, "facing");
    return ladder_facing &&
           blocks.property_value(below, *ladder_facing) == blocks.property_value(feet, *facing);
}

bool feet_in_scaffolding(const CollisionWorld& world, const Vec3d& position) {
    const BlockMotionTable* table = world.motion();
    return table != nullptr && table->of(feet_state(world, position)).scaffolding;
}

Vec3d stuck_multiplier(const CollisionWorld& world, const AABB& box, const Vec3d& position,
                       bool living) {
    const BlockMotionTable* table = world.motion();
    Vec3d                   stuck{};
    if (table == nullptr) {
        return stuck;
    }
    constexpr f64 kInset = 1.0E-7;
    for (i32 x = floor_to_int(box.min.x + kInset); x <= floor_to_int(box.max.x - kInset); ++x) {
        for (i32 y = floor_to_int(box.min.y + kInset); y <= floor_to_int(box.max.y - kInset); ++y) {
            for (i32 z = floor_to_int(box.min.z + kInset); z <= floor_to_int(box.max.z - kInset);
                 ++z) {
                const registry::BlockStateId state  = world.state_at(x, y, z);
                const BlockMotion&           motion = table->of(state);
                if (motion.stuck.x == 0.0 && motion.stuck.y == 0.0 && motion.stuck.z == 0.0) {
                    continue;
                }
                if (motion.stuck_living_only && !living) {
                    continue;
                }
                // Powder snow holds a living thing only by the feet, and
                // anything else wherever it touches.
                if (motion.stuck_needs_feet && living &&
                    world.blocks().block_of(feet_state(world, position)) !=
                        world.blocks().block_of(state)) {
                    continue;
                }
                stuck = motion.stuck;
            }
        }
    }
    return stuck;
}

void apply_inside_effects(const CollisionWorld& world, const AABB& box, const Vec3d& position,
                          f64 width, bool on_ground, Vec3d& velocity,
                          const BlockEffectConstants& constants) {
    const BlockMotionTable* table = world.motion();
    if (table == nullptr) {
        return;
    }
    const registry::BlockRegistry& blocks = world.blocks();
    constexpr f64                  kInset = 1.0E-7;
    for (i32 x = floor_to_int(box.min.x + kInset); x <= floor_to_int(box.max.x - kInset); ++x) {
        for (i32 y = floor_to_int(box.min.y + kInset); y <= floor_to_int(box.max.y - kInset); ++y) {
            for (i32 z = floor_to_int(box.min.z + kInset); z <= floor_to_int(box.max.z - kInset);
                 ++z) {
                const registry::BlockStateId state  = world.state_at(x, y, z);
                const BlockMotion&           motion = table->of(state);
                if (motion.bubble_column) {
                    const bool down = table->bubble(state) == BubbleColumn::Down;
                    const bool surface =
                        blocks.is_air(blocks.block_of(world.state_at(x, y + 1, z)));
                    if (surface) {
                        velocity.y =
                            down ? std::max(constants.bubble_down_surface_min,
                                            velocity.y - constants.bubble_down_step)
                                 : std::min(constants.bubble_up_surface_max,
                                            velocity.y + constants.bubble_up_surface_step);
                    } else {
                        velocity.y =
                            down ? std::max(constants.bubble_down_inside_min,
                                            velocity.y - constants.bubble_down_step)
                                 : std::min(constants.bubble_up_inside_max,
                                            velocity.y + constants.bubble_up_inside_step);
                    }
                } else if (motion.honey) {
                    // Sliding: airborne, below the top of the block, falling
                    // faster than the slide, and pressed against a side.
                    if (on_ground || position.y > static_cast<f64>(y) + 0.9375 - 1.0E-7 ||
                        velocity.y >= constants.honey_slide_above) {
                        continue;
                    }
                    const f64 reach = 0.4375 + width / 2.0;
                    const f64 dx    = std::abs(static_cast<f64>(x) + 0.5 - position.x);
                    const f64 dz    = std::abs(static_cast<f64>(z) + 0.5 - position.z);
                    if (dx + 1.0E-7 <= reach && dz + 1.0E-7 <= reach) {
                        continue;
                    }
                    if (velocity.y < constants.honey_slide_fast) {
                        const f64 scale = constants.honey_slide_speed / velocity.y;
                        velocity.x *= scale;
                        velocity.z *= scale;
                    }
                    velocity.y = constants.honey_slide_speed;
                }
            }
        }
    }
}

void land_on(const CollisionWorld& world, const Vec3d& position, const AABB& box, bool living,
             bool suppress_bounce, bool landed, bool on_ground, Vec3d& velocity,
             const BlockEffectConstants& constants) {
    const BlockMotionTable* table = world.motion();
    // The block the landing is on: the one under the feet, 0.2 down.
    const registry::BlockStateId under =
        world.state_at(floor_to_int(position.x), floor_to_int(position.y - 0.2),
                       floor_to_int(position.z));
    (void)box;
    const BlockMotion* motion = table != nullptr ? &table->of(under) : nullptr;
    if (landed) {
        if (motion != nullptr && motion->bounces && !suppress_bounce && velocity.y < 0.0) {
            velocity.y = -velocity.y *
                         (living ? constants.slime_bounce_living : constants.slime_bounce_other);
        } else {
            velocity.y = 0.0;
        }
    }
    if (on_ground && motion != nullptr && motion->bounces && !suppress_bounce) {
        const f64 vertical = std::abs(velocity.y);
        if (vertical < constants.slime_step_below) {
            const f64 scale = constants.slime_step_base + vertical * constants.slime_step_scale;
            velocity.x *= scale;
            velocity.z *= scale;
        }
    }
}

}  // namespace ov::gameplay
