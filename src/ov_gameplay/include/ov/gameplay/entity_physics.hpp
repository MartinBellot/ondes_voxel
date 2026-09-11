// How something that is not a player moves.
//
// The player's numbers were fitted from a real client's position reports
// (docs/PROVENANCE.md). Reusing them for mobs would have been a guess, and a
// plausible one — the worst kind — so they were measured separately, out of the
// game's own `Motion` tag on a mob dropped from three hundred blocks up. The
// answer is in the constants below, along with what it does and does not cover.
//
// This lives in ov_gameplay rather than ov_entity because it needs collision,
// and collision is layer 9. ov_entity is layer 8 and holds state; how that
// state changes is a rule, and rules live here.
#pragma once

#include "ov/entity/entity.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/vec.hpp"

namespace ov::gameplay {

/// The numbers a free-falling entity moves with.
///
/// One struct rather than constants inlined into the step, so that a trace can
/// be fitted against them one at a time and a wrong one shows up in a diff.
struct EntityMotionConstants {
    /// Subtracted from the vertical velocity each tick, **before** drag.
    ///
    /// Measured, not borrowed from the player: a zombie, a cow and an armour
    /// stand were each dropped from y = 300 and their `Motion` tag sampled
    /// forty times during the fall. Fitting (g, d) to those samples gives
    /// 0.08000 and 0.98000 for all three, with a residual of 1.6e-06 — which is
    /// the sampling quantisation and not a disagreement.
    ///
    /// The order matters as much as the value: drag before gravity gives a
    /// different fall, and both look right.
    f64 gravity{0.08};

    /// Multiplies the vertical velocity each tick, after gravity.
    ///
    /// Together these two give a terminal speed of -g·d/(1-d) = -3.92 blocks a
    /// tick, which is the number a long fall converges on and the fastest speed
    /// observed in the measurement (-3.58 after forty samples, still climbing).
    f64 vertical_drag{0.98};

    /// Multiplies the horizontal velocity each tick while airborne.
    f64 air_drag{0.91};

    /// Ordinary ground. Ice and slime differ, and are per block — not yet read
    /// here, which is stated rather than hidden: a mob sliding on ice will not.
    f64 default_slipperiness{0.6};

    /// A velocity component below this is set to zero. Without it a mob that
    /// has stopped keeps drifting by ever smaller amounts forever, and every
    /// broadcast carries a different number.
    f64 negligible_speed{0.003};
};

/// Advance one entity by one tick of gravity, drag and collision.
///
/// Returns the new state: position, velocity and on_ground. Nothing else about
/// the entity is touched, and no behaviour is applied — an entity that wants to
/// walk sets its own horizontal velocity first.
///
/// The box comes from the entity's own measured width and height, so a chicken
/// lands on top of a fence post and an enderman does not fit under one.
[[nodiscard]] entity::EntityState step_entity(const entity::EntityState&   state,
                                              const EntityMotionConstants& constants,
                                              const CollisionWorld&        world);

/// The box an entity of this size occupies at this position.
[[nodiscard]] AABB entity_box(const entity::EntityState& state) noexcept;

/// The horizontal drag `step_entity` applies this tick: the air's 0.91, times
/// the supporting block's friction on the ground.
[[nodiscard]] f64 entity_friction(const entity::EntityState&   state,
                                  const EntityMotionConstants& constants,
                                  const CollisionWorld&        world);

/// How a walk's cruising speed on the floor under `state` compares with the
/// same walk on ordinary ground — 1 on grass, a little under on ice, 0.58 on
/// soul sand. A mob that sets its own velocity multiplies by this.
[[nodiscard]] f64 walk_floor_scale(const entity::EntityState&   state,
                                   const EntityMotionConstants& constants,
                                   const CollisionWorld&        world);

/// A dropped stack falls at **half** the gravity of everything else.
///
/// Measured the same way and the fit is exact — residual 1.2e-15, which is
/// double-precision noise — so this is not a rounding of 0.08. Its terminal
/// speed is -1.96 blocks a tick rather than -3.92, and using the ordinary
/// constants makes every dropped item reach the ground twice as fast as the
/// game's.
[[nodiscard]] inline EntityMotionConstants item_motion() noexcept {
    EntityMotionConstants constants;
    constants.gravity = 0.04;
    return constants;
}

// Projectiles are **not** covered by this model. An arrow dropped the same way
// fits (g, d) three orders of magnitude worse than a mob does — residual
// 2.6e-03 against 1.6e-06 — so it is moving under different rules, and those
// rules are not implemented here. Stepping an arrow with `step_entity` would
// produce a plausible arc and the wrong one.

}  // namespace ov::gameplay
