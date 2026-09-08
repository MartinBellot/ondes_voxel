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

MotionState step(const MotionState& state, const MoveInput& input, const MotionConstants& constants,
                 const CollisionWorld& world) {
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
