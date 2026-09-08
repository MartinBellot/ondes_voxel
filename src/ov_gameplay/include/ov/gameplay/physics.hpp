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
/// Which fluid fills a block, and how much of it stands there.
///
/// Height is the *rendered* height, (level + 1) / 9 of a block for a flowing
/// source — not the block's own height. It decides two things and nothing
/// else: whether a jump is a jump or a stroke upward, and whether lava is
/// shallow or deep.
enum class Fluid : u8 { None, Water, Lava };

struct FluidSample {
    Fluid fluid{Fluid::None};
    f64   height{0.0};
};

/// How the caller reads fluids, for the same reason CollisionWorld takes a
/// function pointer: this header is public and must not learn what a world is.
using FluidLookup = FluidSample (*)(void* context, i32 x, i32 y, i32 z);

class FluidWorld {
public:
    FluidWorld(FluidLookup lookup, void* context) noexcept : lookup_{lookup}, context_{context} {}

    /// The fluid a player standing at `position` is in, and how deep.
    ///
    /// Vanilla tests the whole bounding box deflated by a thousandth on every
    /// axis, and a block counts when the top of its fluid reaches the bottom of
    /// the box. Anything simpler — the feet block, say — puts a player swimming
    /// while their head is in the air.
    [[nodiscard]] FluidSample sample(const Vec3d& position) const;

private:
    FluidLookup lookup_{nullptr};
    void*       context_{nullptr};
};

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

    // ── Fluids ──────────────────────────────────────────────────────────────
    //
    // Every one of these was checked against a published measurement rather
    // than taken on trust. The terminal speed of a movement under a per-tick
    // drag is a*0.98/(1-drag), and the table below reproduces the game's own
    // published figures to four significant figures — swimming 1.960 against
    // 1.97, sprint-swimming 3.920 against 3.918, sinking in lava 0.8 exactly.

    /// Horizontal drag in water, and the higher one that swimming gives.
    f64 water_drag{0.8};
    f64 swim_drag{0.9};
    /// Vertical drag in water. Unaffected by sprinting or by Depth Strider.
    f64 water_vertical_drag{0.8};
    /// The push an input gets in a fluid. A fifth of walking, which is why
    /// water is slow rather than merely draggy.
    f64 fluid_acceleration{0.02};
    /// Gravity in water: a sixteenth of the usual. This is a 1.13 change — the
    /// older value was a flat 0.02 — and using the old one makes a player sink
    /// four times too fast.
    f64 water_gravity{0.08 / 16.0};
    /// Holding jump in water adds this; sneaking subtracts it. An add, not a
    /// set: buoyancy at the surface is what emerges from this against the
    /// vertical drag, not a special case.
    f64 swim_impulse{0.04};
    /// Above this fluid height a jump becomes a stroke upward. It is also what
    /// separates shallow lava from deep.
    f64 fluid_jump_threshold{0.4};

    /// Lava is thicker in every direction and pulls four times harder than
    /// water.
    f64 lava_drag{0.5};
    f64 lava_shallow_vertical_drag{0.8};
    f64 lava_gravity{0.08 / 4.0};

    /// A velocity component below this is set to zero.
    ///
    /// It was 0.005 before 1.9 and has been 0.003 since. Without it a player
    /// who stops walking drifts by ever smaller amounts forever and every
    /// position report carries a different number.
    f64 negligible_speed{0.003};
};

/// Advance one tick.
///
/// Returns the new state. The collision world decides how far the movement
/// actually gets; the constants decide how far it wanted to go.
/// One tick of movement.
///
/// `fluids` may be null, in which case the player is treated as being in air
/// everywhere — which is what the renderer's free camera wants and what every
/// test that is not about swimming wants.
[[nodiscard]] MotionState step(const MotionState& state, const MoveInput& input,
                               const MotionConstants& constants, const CollisionWorld& world,
                               const FluidWorld* fluids = nullptr);

}  // namespace ov::gameplay
