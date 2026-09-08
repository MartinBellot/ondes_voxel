#include "ov/gameplay/physics.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::gameplay {
namespace {

/// The input, rotated to face the player's yaw and scaled.
///
/// Vanilla normalises only when the input is longer than one, so holding
/// forward alone gives 1 and forward-and-strafe gives the diagonal — walking
/// diagonally is not faster, which a plain scaling would make it.
[[nodiscard]] Vec3d input_vector(const MoveInput& input, f64 amount) {
    const f64 forward_in = static_cast<f64>(input.forward);
    const f64 strafe_in  = static_cast<f64>(input.strafe);
    const f64 length     = std::sqrt(forward_in * forward_in + strafe_in * strafe_in);
    if (length < 1.0E-7) {
        return Vec3d{};
    }
    const f64 scale   = (length > 1.0 ? 1.0 / length : 1.0) * amount;
    const f64 forward = forward_in * scale;
    const f64 strafe  = strafe_in * scale;

    const f64 radians = static_cast<f64>(input.yaw) * std::numbers::pi / 180.0;
    const f64 sin_yaw = std::sin(radians);
    const f64 cos_yaw = std::cos(radians);
    return Vec3d{strafe * cos_yaw - forward * sin_yaw, 0.0, forward * cos_yaw + strafe * sin_yaw};
}

}  // namespace

FluidSample FluidWorld::sample(const Vec3d& position) const {
    FluidSample deepest;
    if (lookup_ == nullptr) {
        return deepest;
    }
    // Vanilla deflates the box by a thousandth on every axis before asking, so
    // that standing exactly against a wall of water does not count as being in
    // it. Reproduced rather than rounded away: without it a player walking
    // along a shoreline flickers in and out of swimming.
    constexpr f64 kShrink = 0.001;
    const AABB    box     = player_box(position);

    const auto floor_i = [](f64 value) {
        const auto floored = static_cast<i32>(value);
        return value < static_cast<f64>(floored) ? floored - 1 : floored;
    };

    const i32 min_x = floor_i(box.min.x + kShrink);
    const i32 max_x = floor_i(box.max.x - kShrink);
    const i32 min_y = floor_i(box.min.y + kShrink);
    const i32 max_y = floor_i(box.max.y - kShrink);
    const i32 min_z = floor_i(box.min.z + kShrink);
    const i32 max_z = floor_i(box.max.z - kShrink);

    for (i32 y = min_y; y <= max_y; ++y) {
        for (i32 z = min_z; z <= max_z; ++z) {
            for (i32 x = min_x; x <= max_x; ++x) {
                const FluidSample here = lookup_(context_, x, y, z);
                if (here.fluid == Fluid::None) {
                    continue;
                }
                // The top of the fluid has to reach the bottom of the box for
                // the block to count at all.
                const f64 top = static_cast<f64>(y) + here.height;
                if (top < box.min.y + kShrink) {
                    continue;
                }
                // Lava wins over water: a player in both is burning, not
                // swimming.
                const f64 depth = top - (box.min.y + kShrink);
                if (deepest.fluid == Fluid::None || here.fluid == Fluid::Lava ||
                    (here.fluid == deepest.fluid && depth > deepest.height)) {
                    deepest.fluid  = here.fluid;
                    deepest.height = depth;
                }
            }
        }
    }
    return deepest;
}

/// The water and lava branches, which replace the whole land tick rather than
/// adjusting it.
///
/// Worth stating plainly because it is the shape of the thing: in a fluid there
/// is no slipperiness, no walk speed, no sprint multiplier and no jump — only
/// an acceleration of a fiftieth, a drag, and a much smaller gravity. A player
/// in water moves like a player in water and not like a slowed-down walker.
[[nodiscard]] MotionState step_in_fluid(const MotionState& state, const MoveInput& input,
                                        const MotionConstants& constants,
                                        const CollisionWorld& world, const FluidSample& fluid) {
    MotionState next  = state;
    const bool  water = fluid.fluid == Fluid::Water;

    // Holding jump is a stroke upward, and sneaking is a stroke down. An add
    // rather than a set, which is what makes bobbing at the surface emerge
    // from the drag instead of needing a case of its own.
    if (input.jump) {
        next.velocity.y += constants.swim_impulse;
    }
    if (input.sneak) {
        next.velocity.y -= constants.swim_impulse;
    }

    // Sprinting in water is the swimming pose: less drag, and no gravity at
    // all while it lasts.
    const f64 horizontal_drag =
        water ? (input.sprint ? constants.swim_drag : constants.water_drag) : constants.lava_drag;
    const bool deep_lava = !water && fluid.height > constants.fluid_jump_threshold;
    const f64  vertical_drag = water ? constants.water_vertical_drag
                                     : (deep_lava ? constants.lava_drag
                                                  : constants.lava_shallow_vertical_drag);

    const Vec3d push = input_vector(input, constants.fluid_acceleration * constants.input_scale);
    next.velocity.x += push.x;
    next.velocity.z += push.z;

    const AABB  box     = player_box(next.position);
    const Vec3d allowed = world.slide(box, next.velocity);
    next.position.x += allowed.x;
    next.position.y += allowed.y;
    next.position.z += allowed.z;

    next.on_ground = allowed.y != next.velocity.y && next.velocity.y < 0.0;
    if (allowed.x != next.velocity.x) {
        next.velocity.x = 0.0;
    }
    if (allowed.z != next.velocity.z) {
        next.velocity.z = 0.0;
    }
    if (allowed.y != next.velocity.y) {
        next.velocity.y = 0.0;
    }

    next.velocity.x *= horizontal_drag;
    next.velocity.y *= vertical_drag;
    next.velocity.z *= horizontal_drag;

    if (water) {
        // Skipped entirely while sprinting: a swimming player gets no downward
        // pull at all, which is why sprint-swimming holds a line.
        if (!input.sprint) {
            next.velocity.y -= constants.water_gravity;
        }
    } else {
        // A quarter of the usual gravity, and *only* that — not water's
        // sixteenth as well. Applying both settles a sinking player at one
        // metre a second where the game's published figure is 0.8, which is
        // how this was caught.
        next.velocity.y -= constants.lava_gravity;
    }

    return next;
}

MotionState step(const MotionState& state, const MoveInput& input, const MotionConstants& constants,
                 const CollisionWorld& world, const FluidWorld* fluids) {
    if (fluids != nullptr) {
        const FluidSample fluid = fluids->sample(state.position);
        // On the ground in shallow water a jump is still a jump. That is the
        // whole job of the threshold, and it is why wading through a puddle
        // does not turn into swimming.
        const bool shallow_enough = fluid.height <= constants.fluid_jump_threshold;
        if (fluid.fluid != Fluid::None && !(state.on_ground && shallow_enough)) {
            return step_in_fluid(state, input, constants, world, fluid);
        }
    }

    MotionState next = state;

    // The block underfoot decides how much of last tick's speed survives. Ice
    // and slime differ, and they are read per block rather than assumed.
    const f64 slipperiness = constants.default_slipperiness;
    const f64 friction = state.on_ground ? slipperiness * constants.air_drag : constants.air_drag;

    // Jumping happens before anything else moves, and a sprinting jump also
    // gets a shove in the direction faced — which is what makes sprint-jumping
    // the fastest way to travel on foot.
    if (input.jump && state.on_ground) {
        next.velocity.y = constants.jump_power;
        if (input.sprint) {
            const f64 radians = static_cast<f64>(input.yaw) * std::numbers::pi / 180.0;
            next.velocity.x -= std::sin(radians) * constants.sprint_jump_boost;
            next.velocity.z += std::cos(radians) * constants.sprint_jump_boost;
        }
        next.on_ground = false;
    }

    // One multiplier covers walking, sprinting and sneaking, and the input
    // scale multiplies all three. That last term is the one nobody
    // reconstructs from first principles and the one that decides the answer:
    // without it a walk settles at 0.2203 blocks a tick instead of 0.2158,
    // which is four per cent fast and looks perfectly reasonable.
    f64 multiplier = constants.input_scale;
    if (input.sprint) {
        multiplier *= constants.sprint_multiplier;
    }
    if (input.sneak) {
        multiplier *= constants.sneak_multiplier;
    }

    // On the ground the push is divided by the cube of the slipperiness, which
    // is what makes ice both slippery and slow to build speed on. In the air it
    // is a smaller number of its own rather than a scaling of the ground one.
    const f64   ratio        = constants.default_slipperiness / slipperiness;
    const f64   acceleration = next.on_ground
                                   ? constants.walk_speed * multiplier * ratio * ratio * ratio
                                   : constants.air_acceleration * multiplier;
    const Vec3d push         = input_vector(input, acceleration);
    next.velocity.x += push.x;
    next.velocity.z += push.z;

    // Move, then let the world cut it short. What was cut is lost: hitting a
    // wall does not store speed for later.
    const AABB  box     = player_box(next.position);
    const Vec3d allowed = world.slide(box, next.velocity);

    next.position.x += allowed.x;
    next.position.y += allowed.y;
    next.position.z += allowed.z;

    next.on_ground = allowed.y != next.velocity.y && next.velocity.y < 0.0;
    if (allowed.x != next.velocity.x) {
        next.velocity.x = 0.0;
    }
    if (allowed.z != next.velocity.z) {
        next.velocity.z = 0.0;
    }
    if (allowed.y != next.velocity.y) {
        next.velocity.y = 0.0;
    }

    // Gravity after the move, then drag — in that order. Drag before gravity
    // gives a different fall, and both look plausible until the numbers are
    // compared against a trace.
    next.velocity.y -= constants.gravity;
    next.velocity.y *= constants.vertical_drag;
    next.velocity.x *= friction;
    next.velocity.z *= friction;

    return next;
}

}  // namespace ov::gameplay
