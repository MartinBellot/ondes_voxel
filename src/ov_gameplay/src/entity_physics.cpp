#include "ov/gameplay/entity_physics.hpp"

#include "ov/gameplay/block_motion.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {

AABB entity_box(const entity::EntityState& state) noexcept {
    return AABB::from_entity(state.position, static_cast<f64>(state.width),
                             static_cast<f64>(state.height));
}

entity::EntityState step_entity(const entity::EntityState&   state,
                                const EntityMotionConstants& constants,
                                const CollisionWorld&        world) {
    entity::EntityState next = state;

    // Gravity first, then drag. The other order gives a fall that is wrong by
    // one tick of gravity forever, and looks entirely plausible.
    next.velocity.y = (next.velocity.y - constants.gravity) * constants.vertical_drag;

    // Horizontal drag. On the ground the supporting block contributes its own
    // slipperiness, read from the world's BlockMotionTable when it has one.
    const f64 friction = entity_friction(state, constants, world);
    next.velocity.x *= friction;
    next.velocity.z *= friction;

    const bool blocky = world.motion() != nullptr;
    const BlockEffectConstants effects;
    // A ladder clamps the speed before the move.
    if (blocky && on_climbable(world, state.position)) {
        const f64 clamp   = effects.climb_clamp;
        next.velocity.x   = std::clamp(next.velocity.x, -clamp, clamp);
        next.velocity.z   = std::clamp(next.velocity.z, -clamp, clamp);
        next.velocity.y   = std::max(next.velocity.y, -clamp);
    }

    if (std::abs(next.velocity.x) < constants.negligible_speed) {
        next.velocity.x = 0.0;
    }
    if (std::abs(next.velocity.y) < constants.negligible_speed) {
        next.velocity.y = 0.0;
    }
    if (std::abs(next.velocity.z) < constants.negligible_speed) {
        next.velocity.z = 0.0;
    }

    const AABB box = entity_box(state);
    // A cobweb scales the move and throws the velocity away.
    Vec3d wanted = next.velocity;
    bool  held   = false;
    if (blocky) {
        const Vec3d stuck = stuck_multiplier(world, box, state.position, true);
        if (stuck.x != 0.0 || stuck.y != 0.0 || stuck.z != 0.0) {
            wanted        = Vec3d{wanted.x * stuck.x, wanted.y * stuck.y, wanted.z * stuck.z};
            next.velocity = wanted;  // what the collision below compares against
            held          = true;
        }
    }
    const Vec3d allowed = world.slide(box, wanted);

    next.position.x += allowed.x;
    next.position.y += allowed.y;
    next.position.z += allowed.z;

    // Grounded means the downward move was cut short — nothing else.
    //
    // A resting entity is not a special case: gravity is applied every tick
    // whether it is standing or not, so a mob on the floor enters every tick
    // with -0.0784 and leaves it with the collision having eaten all of it.
    // Reading on_ground as "velocity is zero" instead would make a mob at the
    // top of a jump count as standing.
    const bool stopped_falling = next.velocity.y < 0.0 && allowed.y > next.velocity.y;
    next.on_ground             = stopped_falling;
    const bool hit_wall        = allowed.x != next.velocity.x || allowed.z != next.velocity.z;

    // Hitting a wall stops the movement into it, rather than letting the
    // velocity build up against it forever.
    if (allowed.x != next.velocity.x) {
        next.velocity.x = 0.0;
    }
    if (allowed.z != next.velocity.z) {
        next.velocity.z = 0.0;
    }

    // A landing stops the fall — or, on slime, sends it back up.
    land_on(world, next.position, box, true, false, stopped_falling, next.on_ground,
            next.velocity, effects);

    if (blocky) {
        const AABB moved = entity_box(next);
        apply_inside_effects(world, moved, next.position, static_cast<f64>(state.width),
                             next.on_ground, next.velocity, effects);
        const f64 speed_factor =
            static_cast<f64>(speed_factor_at(world, next.position, moved, next.on_ground));
        next.velocity.x *= speed_factor;
        next.velocity.z *= speed_factor;
        if (hit_wall && on_climbable(world, next.position)) {
            next.velocity.y = effects.climb_speed;
        }
    }

    // A stuck entity keeps none of its speed: gravity rebuilds it next tick.
    if (held) {
        next.velocity = Vec3d{};
    }

    return next;
}

f64 entity_friction(const entity::EntityState& state, const EntityMotionConstants& constants,
                    const CollisionWorld& world) {
    // In float, as the game computes it: the measured per-tick ratio on stone
    // is float(0.6F × 0.91F) = 0.546000063419, on ice float(0.98F × 0.91F).
    const auto air_drag = static_cast<f32>(constants.air_drag);
    if (!state.on_ground) {
        return static_cast<f64>(air_drag);
    }
    const f32 slipperiness =
        world.motion() != nullptr
            ? floor_friction(world, state.position, entity_box(state), true)
            : static_cast<f32>(constants.default_slipperiness);
    return static_cast<f64>(slipperiness * air_drag);
}

f64 walk_floor_scale(const entity::EntityState& state, const EntityMotionConstants& constants,
                     const CollisionWorld& world) {
    if (!state.on_ground || world.motion() == nullptr) {
        return 1.0;
    }
    const AABB box = entity_box(state);
    const f64  slip =
        static_cast<f64>(floor_friction(world, state.position, box, true));
    const f64 speed = static_cast<f64>(speed_factor_at(world, state.position, box, true));
    const f64 base  = constants.default_slipperiness;
    // Cruise is a / (1 − speed·0.91·f) with a ∝ (0.6 / f)³: the same law as
    // walk_speed.hpp, as a ratio to ordinary ground.
    const f64 ratio = base / slip;
    return ratio * ratio * ratio * (1.0 - constants.air_drag * base) /
           (1.0 - speed * constants.air_drag * slip);
}

}  // namespace ov::gameplay
