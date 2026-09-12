// What a block does to the movement of whatever stands on it or in it.
//
// Ice slides, slime bounces, soul sand drags, a ladder holds, a cobweb stops.
// None of this is in Mojang's reports — it lives in Java code — so every number
// below was read off a real 1.20.1 server: armor stands and dropped items were
// launched across, dropped onto and dropped through each block while a datapack
// recorded their Pos, Motion and OnGround every single tick
// (scripts/measure_block_motion.py, docs/provenance/physique-blocs.md).
//
// The table is built once from the registry, by block name, and then read by
// id: one lookup per tick per entity, never a string comparison in the step.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/vec.hpp"
#include "ov/registry/block_states.hpp"

#include <optional>
#include <vector>

namespace ov::gameplay {

/// Everything one block contributes to movement.
///
/// The three factors are `f32` because the game stores them as floats: a
/// friction of 0.98 is 0.98000001907… and the measured trace carries exactly
/// that, not 0.98. Widened to double where they are used, as the game does.
struct BlockMotion {
    /// Slipperiness. Multiplied by the air's 0.91 for anything on the ground.
    f32 friction{0.6F};
    /// Multiplies horizontal velocity after every move made on (or in) it.
    f32 speed_factor{1.0F};
    /// Multiplies a jump taken from it.
    f32 jump_factor{1.0F};

    /// Inside it, a move is scaled by this and the velocity is thrown away.
    /// Zero on every axis means the block does not hold anything.
    Vec3d stuck{};
    /// Only a living thing is held (sweet berry bushes let an item through).
    bool stuck_living_only{false};
    /// Only held when it is the block at the feet (powder snow).
    bool stuck_needs_feet{false};

    /// `#minecraft:climbable`.
    bool climbable{false};
    /// Scaffolding: climbable, but sneaking does not hold you on it.
    bool scaffolding{false};
    /// A slime block: a landing is reflected rather than stopped.
    bool bounces{false};
    /// A honey block: falling against its side is a slow slide.
    bool honey{false};
    /// A bubble column; `drag` is read from the state.
    bool bubble_column{false};
    /// Soul sand and soul soil — what Soul Speed acts on.
    bool soul_speed_block{false};
    /// Water, or anything a speed factor never comes from (see speed_factor_at).
    bool is_water{false};
};

/// What the bubble column at a position does, if there is one.
enum class BubbleColumn : u8 { None, Up, Down };

class BlockMotionTable {
public:
    explicit BlockMotionTable(const registry::BlockRegistry& blocks);

    [[nodiscard]] const BlockMotion& of(registry::BlockStateId state) const noexcept;

    /// The column's direction: `drag=true` pulls down (magma underneath),
    /// `drag=false` lifts (soul sand underneath).
    [[nodiscard]] BubbleColumn bubble(registry::BlockStateId state) const noexcept;

    [[nodiscard]] const registry::BlockRegistry& blocks() const noexcept { return *blocks_; }

private:
    const registry::BlockRegistry* blocks_{nullptr};
    std::vector<BlockMotion>       by_block_;
    registry::BlockId              bubble_column_{};
    std::optional<registry::PropertyView> drag_{};
};

// ── The constants of the effects themselves ────────────────────────────────
//
// Separate from the per-block table because they belong to the effect and not
// to any block: every climbable clamps to the same speed.

struct BlockEffectConstants {
    /// On a climbable, horizontal and downward speed are clamped to this.
    f64 climb_clamp{static_cast<f64>(0.15F)};
    /// Pushing into a wall (or holding jump) on a climbable sets this upward.
    f64 climb_speed{0.2};

    /// A living thing landing on slime goes back up at the speed it came down;
    /// anything else keeps this much of it.
    f64 slime_bounce_living{1.0};
    f64 slime_bounce_other{0.8};
    /// Walking on slime: horizontal speed is multiplied by base + |vy|·scale
    /// while the vertical speed is below `slime_step_below`.
    f64 slime_step_base{0.4};
    f64 slime_step_scale{0.2};
    f64 slime_step_below{0.1};

    /// Sliding down the side of a honey block.
    f64 honey_slide_speed{-0.05};
    f64 honey_slide_above{-0.08};
    f64 honey_slide_fast{-0.13};

    /// Bubble columns: inside, and at the surface (air above).
    f64 bubble_up_inside_step{0.06};
    f64 bubble_up_inside_max{0.7};
    f64 bubble_up_surface_step{0.1};
    f64 bubble_up_surface_max{1.8};
    f64 bubble_down_step{0.03};
    f64 bubble_down_inside_min{-0.3};
    f64 bubble_down_surface_min{-0.9};

    /// How hard a current pushes, per tick, before the drag. Water everywhere;
    /// lava is slower in the overworld than in the Nether.
    f64 water_push{0.014};
    f64 lava_push{0.0023333333333333335};
    f64 lava_push_ultrawarm{0.007};
    /// A push weaker than this on a nearly still entity is raised to it.
    f64 min_push{0.0045};
};

// ── Reading the blocks around a box ────────────────────────────────────────
//
// Every function below takes the CollisionWorld the step already has, and
// answers "ordinary ground, nothing inside" when that world carries no
// BlockMotionTable — so a caller that never asked for block effects gets
// exactly the physics it had before.

/// The block that decides friction, speed and jump for something standing at
/// `position` with box `box`.
///
/// On the ground it is the **supporting** block — of the blocks whose shapes
/// the bottom of the box rests on, the one whose centre is nearest — at the
/// height half a block below the feet. Standing on the edge of ice over air
/// still slides. In the air it is simply the block half a block below.
[[nodiscard]] registry::BlockStateId movement_floor(const CollisionWorld& world,
                                                    const Vec3d& position, const AABB& box,
                                                    bool on_ground);

/// Friction of that block (0.6 without a table).
[[nodiscard]] f32 floor_friction(const CollisionWorld& world, const Vec3d& position,
                                 const AABB& box, bool on_ground);

/// The feet block's speed factor, or the floor's when the feet block has none.
/// Water and bubble columns answer for themselves and never look below.
[[nodiscard]] f32 speed_factor_at(const CollisionWorld& world, const Vec3d& position,
                                  const AABB& box, bool on_ground);

/// The same rule for the jump factor (honey halves a jump).
[[nodiscard]] f32 jump_factor_at(const CollisionWorld& world, const Vec3d& position,
                                 const AABB& box, bool on_ground);

/// Is the block at the feet climbable — `#minecraft:climbable`, or an open
/// trapdoor over a ladder facing the same way?
[[nodiscard]] bool on_climbable(const CollisionWorld& world, const Vec3d& position);

/// Is the block at the feet scaffolding? (Sneaking does not hold you on it.)
[[nodiscard]] bool feet_in_scaffolding(const CollisionWorld& world, const Vec3d& position);

/// The multiplier a move made this tick is scaled by, from the blocks the box
/// overlaps (deflated by 1e-7): cobweb, sweet berry bush, powder snow. Zero
/// when nothing holds it. The last block in x, y, z order wins.
[[nodiscard]] Vec3d stuck_multiplier(const CollisionWorld& world, const AABB& box,
                                     const Vec3d& position, bool living);

/// What the blocks the box is inside do to the velocity after a move: bubble
/// columns lift or drag, the side of a honey block slows a fall to a slide.
/// Applied once per block overlapped, as the game does.
void apply_inside_effects(const CollisionWorld& world, const AABB& box, const Vec3d& position,
                          f64 width, bool on_ground, Vec3d& velocity,
                          const BlockEffectConstants& constants = {});

/// A landing: the velocity after a downward move was cut short. Slime reflects
/// it unless the bounce is suppressed (a sneaking player); everything else
/// stops. Then walking on slime slows the horizontal speed.
void land_on(const CollisionWorld& world, const Vec3d& position, const AABB& box, bool living,
             bool suppress_bounce, bool landed, bool on_ground, Vec3d& velocity,
             const BlockEffectConstants& constants = {});

}  // namespace ov::gameplay
