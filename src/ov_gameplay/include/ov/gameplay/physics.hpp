// How a player moves.
//
// Every constant here is a measurement, not a recollection. Minecraft's
// movement is a handful of numbers applied in a fixed order, and the order
// matters as much as the values: gravity before drag gives a different fall
// from drag before gravity, and both look plausible.
//
// The reference implementation is the vanilla client, which reports where it is
// twenty times a second. The server records that (`--record-motion`) and the
// constants below are derived from those traces — see docs/PROVENANCE.md.
#pragma once

#include "ov/gameplay/collision.hpp"
#include "ov/math/vec.hpp"

namespace ov::gameplay {

/// What the player is asking for this tick.
struct MoveInput {
    /// Forward and sideways, each in -1..1, before any speed is applied.
    f32 forward{0.0F};
    f32 strafe{0.0F};
    /// Where the player is looking, in degrees. Movement is relative to it.
    f32 yaw{0.0F};

    bool jump{false};
    bool sprint{false};
    bool sneak{false};
};

/// Everything that carries from one tick to the next.
struct MotionState {
    Vec3d position{};
    Vec3d velocity{};
    bool  on_ground{false};
};

/// The numbers vanilla applies, in the order it applies them.
///
/// Named rather than inlined so a trace can be fitted against them one at a
/// time, and so a wrong one is visible in a diff.
struct MotionConstants {
    /// Subtracted from the vertical velocity each tick, before drag.
    f64 gravity{0.08};
    /// Multiplies the vertical velocity each tick, after gravity.
    f64 vertical_drag{0.98};

    /// Multiplies the horizontal velocity each tick. The block underfoot
    /// contributes its own slipperiness on top of this.
    f64 air_drag{0.91};
    /// Ordinary ground. Ice and slime differ, and are per block.
    f64 default_slipperiness{0.6};

    /// The attribute a player walks at, before sprint or sneak.
    f64 walk_speed{0.1};
    f64 sprint_multiplier{1.3};
    f64 sneak_multiplier{0.3};

    /// What the input impulses are scaled by before anything else.
    ///
    /// The term nobody reconstructs from first principles, and the one that
    /// decides the answer: without it a walk settles at 0.2203 blocks a tick
    /// instead of 0.2158, which is four per cent fast and looks right.
    f64 input_scale{0.98};

    /// How hard the player pushes while airborne, before the movement
    /// multiplier. Not derived from the ground figure — it is its own number.
    f64 air_acceleration{0.02};

    /// The upward velocity a jump starts with.
    f64 jump_power{0.42};
    /// A sprinting jump also gets a shove in the direction faced.
    f64 sprint_jump_boost{0.2};

    /// How high a step the player climbs without jumping.
    f64 step_height{0.6};
};

/// Advance one tick.
///
/// Returns the new state. The collision world decides how far the movement
/// actually gets; the constants decide how far it wanted to go.
[[nodiscard]] MotionState step(const MotionState& state, const MoveInput& input,
                               const MotionConstants& constants, const CollisionWorld& world);

}  // namespace ov::gameplay
