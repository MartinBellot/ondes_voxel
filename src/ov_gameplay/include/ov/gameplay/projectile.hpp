// Arrows, tridents, snowballs, eggs, ender pearls and experience bottles.
//
// entity_physics.hpp says a projectile does not follow the mob model — an
// arrow fitted against it left a residual of 2.6e-03 — and this is the model it
// does follow. Everything with a number was measured on a real 1.20.1 server by
// scripts/measure_projectiles.py (docs/provenance/projectiles.md):
//
//   * **Move, then drag, then gravity.** Consecutive samples of a projectile's
//     own `Motion` and `Pos`, labelled by a witness TNT's `Fuse` read in the
//     same command, give one-tick transitions directly. The position advances
//     by the velocity **before** the tick's drag (residual 1.3e-14; with the
//     velocity after, 0.053), and the vertical regression `v' = d·v − g` has
//     intercept −g, not −d·g: gravity comes after the drag.
//
//   * **Every constant is a Java float.** The drag is 0.9900000095367432 —
//     `0.99F` widened — on every axis of every projectile, to sixteen digits.
//     Gravity is `0.05F` for an arrow, a trident and a splash potion, `0.03F`
//     for a snowball, an egg and a pearl, `0.07F` for an experience bottle.
//     In water an arrow drags `0.6F`, a snowball `0.8F`, a trident `0.99F`.
//
// Layer 9: the step reads a `CollisionWorld` and a `LevelView`, and reports
// what it hit into a queue the caller drains. It never hurts anything itself —
// who may be told about a hit, and how a mob's health changes, is the server's.
#pragma once

#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The projectiles this module moves.
enum class ProjectileKind : u8 {
    Arrow,
    SpectralArrow,
    Trident,
    Snowball,
    Egg,
    EnderPearl,
    ExperienceBottle,
    /// A thrown splash or lingering potion. Flies; its effect is not applied.
    Potion,
};

/// The kind for an entity type name ("minecraft:arrow"), or nothing.
[[nodiscard]] std::optional<ProjectileKind> projectile_kind(std::string_view type_name) noexcept;

/// The entity type name of a kind.
[[nodiscard]] std::string_view projectile_type_name(ProjectileKind kind) noexcept;

/// An arrow, a spectral arrow or a trident: the three that stick in a block
/// and can be picked up. The others break on whatever they touch.
[[nodiscard]] constexpr bool is_arrow_like(ProjectileKind kind) noexcept {
    return kind == ProjectileKind::Arrow || kind == ProjectileKind::SpectralArrow ||
           kind == ProjectileKind::Trident;
}

/// How one kind moves. Every number is the `f32` the game stores, widened.
struct ProjectileMotion {
    f64 gravity{static_cast<f64>(0.05F)};
    f64 air_drag{static_cast<f64>(0.99F)};
    f64 water_drag{static_cast<f64>(0.6F)};
    /// Full width and height of the box, from the measured entity table.
    f64 width{0.5};
    f64 height{0.5};
};

/// The measured motion of a kind. See the header comment.
[[nodiscard]] ProjectileMotion projectile_motion(ProjectileKind kind) noexcept;

// ── Ray against the world ───────────────────────────────────────────────────

/// The first block collision box a segment enters.
struct BlockClip {
    Vec3d    point{};
    BlockPos block{};
    /// 0..5 in the protocol's order: -Y, +Y, -Z, +Z, -X, +X — the face that
    /// was entered.
    i32 face{1};
};

/// Where the segment `from → to` first touches a collision box of a block.
/// Uses the same per-state boxes as entity movement, so an arrow flies over a
/// slab and into the top half of a stair.
[[nodiscard]] std::optional<BlockClip> clip_blocks(const CollisionWorld& world, const Vec3d& from,
                                                   const Vec3d& to);

/// Where a segment enters a box, as a fraction of its length, or nothing.
[[nodiscard]] std::optional<f64> clip_box(const AABB& box, const Vec3d& from,
                                          const Vec3d& to) noexcept;

/// Something a projectile may hit that is not a block.
struct ProjectileTarget {
    i32  network_id{0};
    AABB box{};
    /// A player, not an entity of the world. The caller hurts it differently.
    bool player{false};
    /// Cannot be hit at all — a spectator, an item, another projectile.
    bool ignored{false};
};

/// A target's box is grown by this much before the segment is tested against
/// it. From the wiki's Arrow article; the damage campaign hits a cow from a
/// tenth of a block outside the grown box and does not miss.
inline constexpr f64 kTargetInflate = 0.3;

// ── The step ────────────────────────────────────────────────────────────────

/// What one projectile carries besides its entity state.
struct ProjectileData {
    ProjectileKind kind{ProjectileKind::Arrow};
    /// The wire id of whatever shot it; 0 for none (a summoned arrow).
    i32 owner{0};
    /// The owner is a player, not an entity of the world.
    bool owner_is_player{false};
    /// Base damage, multiplied by the speed at impact. 2.0 for an arrow; Power
    /// adds to it. Unused by a thrown item and by a trident, whose hit is 8.
    f64 base_damage{2.0};
    bool critical{false};
    /// 0 no one, 1 anyone who could have shot it, 2 creative players only.
    u8 pickup{0};
    /// Piercing level; 0 for none. Named only: an arrow stops at its first hit.
    u8   pierce{0};
    bool from_crossbow{false};
    /// Punch level.
    u8 punch{0};
    /// Flame / fire: named, this server has no fire on entities.
    bool flaming{false};
    bool in_ground{false};
    /// Ticks spent in the ground. An arrow is removed at 1200.
    i32 life{0};
    /// The block it stuck in, while in the ground.
    BlockPos stuck{};
    registry::BlockStateId stuck_state{};
    /// Ticks since it was shot, for the owner grace.
    i32 age{0};
    /// A trident that has hit something deals no more damage.
    bool dealt_damage{false};
    /// Has the projectile's box left its owner's box yet? Until it has, the
    /// owner cannot be hit — which is how an arrow shot straight up does not
    /// hit the archer on the way out.
    bool left_owner{false};
};

/// What happened during one step, for the caller to act on.
struct ProjectileEvent {
    enum class Kind : u8 {
        /// It hit an entity or a player. The caller hurts it and then tells
        /// the logic whether the hit landed (`resolve_entity_hit`).
        HitEntity,
        /// It stuck in a block (an arrow, a trident).
        Stuck,
        /// It broke on a block or an entity (every thrown item).
        Broke,
        /// It stayed in the ground too long.
        Expired,
    };
    Kind  kind{Kind::HitEntity};
    i32   projectile{0};
    i32   target{0};
    bool  target_is_player{false};
    /// Where the hit was.
    Vec3d point{};
    /// The velocity at the moment of the hit — what the damage is scaled by.
    Vec3d velocity{};
    /// What the projectile was, and who shot it. Carried here because a
    /// projectile that broke is gone by the time the caller reads the event,
    /// and a pearl still has to find its thrower.
    ProjectileKind projectile_kind{ProjectileKind::Arrow};
    i32            owner{0};
    bool           owner_is_player{false};
    /// ── brewing ── Where the projectile was when this tick began. A potion's
    /// splash is measured from here, not from `point`: measured, a potion that
    /// fell through a player and broke on the floor gave 909 of 1000, which is
    /// its height 0.365 at the start of its last tick — the impact would be 0.
    ///
    /// **Last, on purpose.** Events are built with positional initialisers;
    /// placed after `point`, this field took the velocity's place, every event
    /// carried a velocity of 0 and every arrow hit for 0 — which the projectile
    /// tests caught (`0.0 == 0.3`).
    Vec3d from{};
};

/// Where the logic reports, drained by the caller after the entity tick.
struct ProjectileEvents {
    std::vector<ProjectileEvent> events;
};

/// What the logic reads besides the collision world: the targets, the water.
/// The caller fills it before the entity tick and owns it.
struct ProjectileWorld {
    std::vector<ProjectileTarget> targets;
    ProjectileEvents              events;
    /// Water, for the drag: 0 when the registry has no water.
    registry::BlockId water{0};
};

/// One tick of movement, without any logic around it: collide with blocks and
/// with `targets`, move by the old velocity, drag, then gravity.
///
/// Returns the event, if anything was hit. `data.in_ground` projectiles do not
/// move. The entity state's position and velocity are updated in place.
[[nodiscard]] std::optional<ProjectileEvent> step_projectile(entity::EntityState& state,
                                                             ProjectileData&      data,
                                                             const CollisionWorld& world,
                                                             const world::LevelView* level,
                                                             const ProjectileWorld& targets);

/// Is the box in water? Any cell it overlaps that holds water counts.
[[nodiscard]] bool box_in_water(const world::LevelView& level, const AABB& box,
                                registry::BlockId water);

/// The logic of one projectile entity.
class ProjectileLogic final : public entity::IEntityLogic {
public:
    ProjectileLogic(ProjectileData data, ProjectileWorld& world) noexcept
        : data_{data}, world_{&world} {}

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override {
        return projectile_type_name(data_.kind);
    }

    [[nodiscard]] const ProjectileData& data() const noexcept { return data_; }
    [[nodiscard]] ProjectileData&       data() noexcept { return data_; }

private:
    ProjectileData   data_;
    ProjectileWorld* world_;
};

/// After the caller has tried to hurt what an arrow hit: remove it when the
/// hurt landed, send it back the way it came when it did not. Measured: an
/// arrow that meets a mob inside its invulnerability window bounces.
void resolve_entity_hit(entity::EntityState& state, ProjectileData& data, bool hurt_landed) noexcept;

/// An arrow is removed after this long in the ground.
inline constexpr i32 kArrowDespawnTicks = 1200;

// ── Damage ──────────────────────────────────────────────────────────────────

/// What an arrow does to what it hits: `ceil(speed × base)`, plus, for a
/// critical, a draw of `[0, dmg/2 + 2)`. Returns the whole number of points.
///
/// `critical_draw` is called with the exclusive bound and returns the draw;
/// nothing is drawn for an arrow that is not critical.
[[nodiscard]] i32 arrow_damage(const Vec3d& velocity, f64 base_damage, bool critical,
                               math::LegacyRandomSource& random) noexcept;

/// A trident's hit, whatever its speed: 8.
inline constexpr f32 kTridentDamage = 8.0F;

/// A snowball hurts a blaze for 3 and anything else for 0 (with knockback).
[[nodiscard]] f32 snowball_damage(bool target_is_blaze) noexcept;

/// An ender pearl costs the thrower this much when it lands.
inline constexpr f32 kPearlFallDamage = 5.0F;

// ── Shooting ────────────────────────────────────────────────────────────────

/// The direction a shooter with this yaw and pitch (degrees) is looking,
/// computed with the game's float sine table.
[[nodiscard]] Vec3d look_direction(f32 yaw, f32 pitch) noexcept;

/// The velocity a projectile is shot with: `direction` normalised, jittered
/// per axis by a triangular draw of half-width `0.0172275 × inaccuracy`, then
/// scaled to `speed`. Three draws, x then y then z, in that order.
[[nodiscard]] Vec3d shoot_velocity(const Vec3d& direction, f32 speed, f32 inaccuracy,
                                   math::LegacyRandomSource& random) noexcept;

/// A bow's power after `ticks` of drawing: `f = t/20; (f² + 2f)/3`, capped at
/// 1. In floats, as the game computes it. Below 0.1 the bow does not shoot.
[[nodiscard]] f32 bow_power(i32 ticks) noexcept;

/// The arrow leaves the bow at `3 × power`, critical at full power.
inline constexpr f32 kBowSpeed        = 3.0F;
inline constexpr f32 kBowInaccuracy   = 1.0F;
inline constexpr f32 kBowMinimumPower = 0.1F;

/// A crossbow charges in 25 ticks, 5 fewer per Quick Charge level.
[[nodiscard]] i32 crossbow_charge_ticks(u8 quick_charge) noexcept;

/// A crossbow's arrow leaves at 3.15.
inline constexpr f32 kCrossbowSpeed      = 3.15F;
inline constexpr f32 kCrossbowInaccuracy = 1.0F;

/// A trident must be drawn this long before it is thrown, and leaves at 2.5.
inline constexpr i32 kTridentMinimumTicks = 10;
inline constexpr f32 kTridentSpeed        = 2.5F;

/// A snowball, an egg and a pearl are thrown at 1.5; a bottle at 0.7, twenty
/// degrees above the look.
inline constexpr f32 kThrowSpeed         = 1.5F;
inline constexpr f32 kBottleSpeed        = 0.7F;
inline constexpr f32 kBottlePitchOffset  = -20.0F;
inline constexpr f32 kThrowInaccuracy    = 1.0F;

/// Where a shot starts: the shooter's eyes, a tenth of a block lower.
inline constexpr f64 kShotBelowEye = 0.1;

/// A skeleton shoots at 1.6, with an inaccuracy of `14 − 4 × difficulty`.
inline constexpr f32 kSkeletonArrowSpeed = 1.6F;
[[nodiscard]] f32 skeleton_inaccuracy(i32 difficulty) noexcept;

/// The velocity a skeleton gives its arrow at a target: aimed at a third of
/// the target's height, lifted by a fifth of the horizontal distance.
[[nodiscard]] Vec3d skeleton_aim(const Vec3d& arrow_start, const Vec3d& target_feet,
                                 f64 target_height, i32 difficulty,
                                 math::LegacyRandomSource& random) noexcept;

// ── Eggs ────────────────────────────────────────────────────────────────────

/// How many chickens an egg that breaks hatches: none seven times in eight;
/// otherwise one, or four one time in thirty-two. Two draws, in that order.
[[nodiscard]] i32 egg_chickens(math::LegacyRandomSource& random) noexcept;

/// Items and things this module recognises and does not carry out, named
/// rather than approximated. For the server's log at start-up.
[[nodiscard]] std::span<const std::string_view> projectile_gaps() noexcept;

}  // namespace ov::gameplay
