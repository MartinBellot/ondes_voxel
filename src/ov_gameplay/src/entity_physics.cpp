#include "ov/gameplay/entity_physics.hpp"

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

    // Horizontal drag. On the ground the block underfoot contributes its own
    // slipperiness; only the default is applied here, and a mob will not slide
    // on ice until the block is read — said rather than silently approximated.
    const f64 friction =
        state.on_ground ? constants.air_drag * constants.default_slipperiness : constants.air_drag;
    next.velocity.x *= friction;
    next.velocity.z *= friction;

    if (std::abs(next.velocity.x) < constants.negligible_speed) {
        next.velocity.x = 0.0;
    }
    if (std::abs(next.velocity.y) < constants.negligible_speed) {
        next.velocity.y = 0.0;
    }
    if (std::abs(next.velocity.z) < constants.negligible_speed) {
        next.velocity.z = 0.0;
    }

    const AABB  box     = entity_box(state);
    const Vec3d allowed = world.slide(box, next.velocity);

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
    if (stopped_falling) {
        next.velocity.y = 0.0;
    }

    // Hitting a wall stops the movement into it, rather than letting the
    // velocity build up against it forever.
    if (allowed.x != next.velocity.x) {
        next.velocity.x = 0.0;
    }
    if (allowed.z != next.velocity.z) {
        next.velocity.z = 0.0;
    }

    return next;
}

}  // namespace ov::gameplay
