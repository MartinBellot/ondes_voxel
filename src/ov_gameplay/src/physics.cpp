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

/// Below this, a velocity is zero.
///
/// Vanilla drops any component under the threshold to nothing rather than
/// letting it decay forever. Without it a player who stops walking keeps
/// drifting by ever smaller amounts and never quite arrives, and every position
/// report carries a different number.
void clamp_negligible(Vec3d& velocity, const MotionConstants& constants) {
    if (std::abs(velocity.x) < constants.negligible_speed) {
        velocity.x = 0.0;
    }
    if (std::abs(velocity.y) < constants.negligible_speed) {
        velocity.y = 0.0;
    }
    if (std::abs(velocity.z) < constants.negligible_speed) {
        velocity.z = 0.0;
    }
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

    const AABB  box      = player_box(next.position);
    const Vec3d allowed  = world.slide(box, next.velocity);
    const bool  hit_wall = allowed.x != next.velocity.x || allowed.z != next.velocity.z;
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

    // Bubble columns act on the velocity after the move and before the drag;
    // a ladder under water still climbs when walked into.
    if (world.motion() != nullptr) {
        apply_inside_effects(world, player_box(next.position), next.position, 0.6,
                             next.on_ground, next.velocity, constants.effects);
        if (hit_wall && on_climbable(world, next.position)) {
            next.velocity.y = constants.effects.climb_speed;
        }
    }

    next.velocity.x *= horizontal_drag;
    next.velocity.y *= vertical_drag;
    next.velocity.z *= horizontal_drag;
    clamp_negligible(next.velocity, constants);

    if (water) {
        // Skipped entirely while sprinting: a swimming player gets no downward
        // pull at all, which is why sprint-swimming holds a line.
        if (!input.sprint) {
            next.velocity.y -= constants.water_gravity;
        }
    } else {
        // Deep lava gets a quarter of gravity and nothing else — 0.02 over a
        // drag of 0.5 is exactly the published 0.8 m/s, and adding water's
        // sixteenth on top gave 1.0, which is how the first version was
        // caught.
        //
        // Shallow lava gets *both*, and that is not a contradiction: it is a
        // separate branch with a different drag. Two independent
        // reimplementations agree that the shallow case runs the same falling
        // adjustment water does before the quarter is applied.
        if (!deep_lava && !input.sprint) {
            next.velocity.y -= constants.water_gravity;
        }
        next.velocity.y -= constants.lava_gravity;
    }

    return next;
}

/// Creative flight, which replaces the land tick the way a fluid does.
///
/// The same shape as swimming and read back the same way, from published
/// speeds through the terminal-speed arithmetic a·0.98/(1−drag): no gravity,
/// the horizontal drag of air, a vertical drag of its own, and a push equal to
/// the flying speed the server grants. See MotionConstants for the figures.
[[nodiscard]] MotionState step_flying(const MotionState& state, const MoveInput& input,
                                      const MotionConstants& constants,
                                      const CollisionWorld& world) {
    MotionState next  = state;
    const f64   speed = static_cast<f64>(input.flying_speed);

    // Jump climbs and sneak descends — an add before the move rather than a
    // set, so a tap is a nudge and holding the key settles at a speed.
    const f64 climb = constants.flying_vertical_factor * speed;
    if (input.jump) {
        next.velocity.y += climb;
    }
    if (input.sneak) {
        next.velocity.y -= climb;
    }

    // Sneaking does not slow a flier down: the key is taken by the descent.
    const f64 multiplier = input.sprint ? constants.flying_sprint_multiplier : 1.0;
    const Vec3d push     = input_vector(input, speed * multiplier * constants.input_scale);
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

    next.velocity.x *= constants.air_drag;
    next.velocity.z *= constants.air_drag;
    next.velocity.y *= constants.flying_vertical_drag;
    clamp_negligible(next.velocity, constants);
    return next;
}

MotionState step(const MotionState& state, const MoveInput& input, const MotionConstants& constants,
                 const CollisionWorld& world, const FluidWorld* fluids) {
    if (input.flying) {
        return step_flying(state, input, constants, world);
    }
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

    MotionState       next      = state;
    const AABB        start_box = player_box(state.position);
    const bool        blocky    = world.motion() != nullptr;
    const BlockEffectConstants& effects = constants.effects;

    // The block underfoot decides how much of last tick's speed survives: the
    // supporting block, half a block below the feet, read before the move.
    const f64 slipperiness =
        blocky ? static_cast<f64>(floor_friction(world, state.position, start_box, state.on_ground))
               : constants.default_slipperiness;
    // The product is taken in float, as the game does: an armor stand on stone
    // keeps 0.546000063419 of its speed per tick — float(0.6F × 0.91F) — and
    // not the 0.546000021696 of the same product in double.
    const auto air_drag = static_cast<f32>(constants.air_drag);
    const f64  friction = state.on_ground
                              ? static_cast<f64>(static_cast<f32>(slipperiness) * air_drag)
                              : static_cast<f64>(air_drag);

    // Jumping happens before anything else moves, and a sprinting jump also
    // gets a shove in the direction faced — which is what makes sprint-jumping
    // the fastest way to travel on foot. Honey halves the jump, not the shove.
    if (input.jump && state.on_ground) {
        const f64 jump_factor =
            blocky ? static_cast<f64>(jump_factor_at(world, state.position, start_box, true)) : 1.0;
        next.velocity.y = constants.jump_power * jump_factor;
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

    // On a climbable the speed is clamped before the move, and holding sneak
    // stops the slide down — except on scaffolding, where sneaking is how you
    // go down.
    if (blocky && on_climbable(world, state.position)) {
        const f64 clamp = effects.climb_clamp;
        next.velocity.x = std::clamp(next.velocity.x, -clamp, clamp);
        next.velocity.z = std::clamp(next.velocity.z, -clamp, clamp);
        next.velocity.y = std::max(next.velocity.y, -clamp);
        if (next.velocity.y < 0.0 && input.sneak && !feet_in_scaffolding(world, state.position)) {
            next.velocity.y = 0.0;
        }
    }

    // Inside a cobweb, a berry bush or powder snow the move is scaled and the
    // velocity thrown away: only what gravity rebuilds this tick moves you.
    Vec3d wanted = next.velocity;
    if (blocky) {
        const Vec3d stuck = stuck_multiplier(world, start_box, state.position, true);
        if (stuck.x != 0.0 || stuck.y != 0.0 || stuck.z != 0.0) {
            wanted        = Vec3d{wanted.x * stuck.x, wanted.y * stuck.y, wanted.z * stuck.z};
            next.velocity = Vec3d{};
        }
    }

    // Move, then let the world cut it short. What was cut is lost: hitting a
    // wall does not store speed for later.
    const AABB  box     = player_box(next.position);
    const Vec3d allowed = world.slide(box, wanted);

    next.position.x += allowed.x;
    next.position.y += allowed.y;
    next.position.z += allowed.z;

    const bool landed      = allowed.y != wanted.y && wanted.y < 0.0;
    const bool hit_wall    = allowed.x != wanted.x || allowed.z != wanted.z;
    next.on_ground         = landed;
    if (allowed.x != wanted.x) {
        next.velocity.x = 0.0;
    }
    if (allowed.z != wanted.z) {
        next.velocity.z = 0.0;
    }
    if (allowed.y != wanted.y && !landed) {
        next.velocity.y = 0.0;  // a ceiling
    }
    // A landing stops the fall — or, on slime, reflects it.
    land_on(world, next.position, box, true, input.sneak, landed, next.on_ground, next.velocity,
            effects);

    if (blocky) {
        const AABB moved = player_box(next.position);
        apply_inside_effects(world, moved, next.position, 0.6, next.on_ground, next.velocity,
                             effects);
        const f64 speed_factor =
            static_cast<f64>(speed_factor_at(world, next.position, moved, next.on_ground));
        next.velocity.x *= speed_factor;
        next.velocity.z *= speed_factor;
        // Climbing is walking into a wall (or holding jump) on a climbable.
        if ((hit_wall || input.jump) && on_climbable(world, next.position)) {
            next.velocity.y = effects.climb_speed;
        }
    }

    // Gravity after the move, then drag — in that order. Drag before gravity
    // gives a different fall, and both look plausible until the numbers are
    // compared against a trace.
    next.velocity.y -= constants.gravity;
    next.velocity.y *= constants.vertical_drag;
    next.velocity.x *= friction;
    next.velocity.z *= friction;

    clamp_negligible(next.velocity, constants);
    return next;
}

}  // namespace ov::gameplay
