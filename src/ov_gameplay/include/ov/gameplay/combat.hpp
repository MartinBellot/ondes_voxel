// Hitting things: what a weapon does, and how much of it the swing was worth.
//
// Melee in 1.9 and after is not "damage per click". A swing carries a
// **charge** — a fraction of the weapon's own cooldown that has elapsed since
// the last one — and the damage is scaled by it. A player who clicks as fast as
// they can does a fifth of the damage of one who waits, which is the single
// rule that separates modern combat from the version before it, and an
// implementation without it plays like a different game while looking correct
// in every screenshot.
//
// Everything numeric here was measured against a real 1.20.1 server by
// scripts/measure_combat.py, two independent ways wherever there were two:
//
//   * The weapon table is the `generic.attack_damage` and `generic.attack_speed`
//     attributes read off a player holding each item — the item's own modifiers
//     are folded into the player's attribute map, so the console answers with
//     the number the game will use. All 34 were then *hit into a sheep* at full
//     charge and the health removed matched the attribute exactly, item for
//     item.
//
//   * The gauge was sampled every half tick from zero to forty ticks and the
//     damage read back. It is a staircase, one step per tick, and it lands on
//     `0.2 + 0.8 * f²` with `f = (ticks + 0.5) / cooldown` clamped to one — the
//     half tick is not a rounding choice, it is in the measurement: a swing
//     made on the same tick as the last one already does 1.4089 rather than
//     1.4 of a diamond sword's seven.
//
// This is layer 9 and it stays there. Nothing here takes a level, a server or
// an entity: a caller passes state in and gets a decision back, which is what
// lets the same code run inside a client's prediction.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/vec.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// What one item does to the two attack attributes.
///
/// Stored as the **total** a player holding it has, not as the modifier: the
/// modifier is meaningless without the base, and the base is 1.0 damage and 4.0
/// speed for a bare hand — which is itself a measured row of this table rather
/// than an assumption.
struct WeaponStats {
    /// The registry name, e.g. "minecraft:diamond_sword".
    std::string_view item;

    /// `generic.attack_damage`, the damage a fully charged hit removes.
    f32 attack_damage;

    /// `generic.attack_speed`, in swings per second. The cooldown is twenty
    /// divided by it, and it is not always an integer number of ticks: 1.6 for
    /// every sword gives 12.5.
    f32 attack_speed;
};

/// The measured table, in the order the campaign walked it.
[[nodiscard]] std::span<const WeaponStats> weapon_table() noexcept;

/// What a player holding this item attacks with.
///
/// An item with no attack modifiers — a stick, a block, an empty hand — gives
/// the bare-hand row, and that is measured rather than assumed: `stick`,
/// `shears` and an empty hand all answered 1.0 damage and 4.0 speed on a real
/// server. So the fallback here is the game's own rule, not a default standing
/// in for a missing entry.
[[nodiscard]] WeaponStats weapon_for(std::string_view item) noexcept;

/// Twenty ticks divided by the attack speed.
[[nodiscard]] f32 attack_cooldown_ticks(f32 attack_speed) noexcept;

/// The charge of a swing made `ticker` ticks after the previous one.
///
/// `(ticker + 0.5) / cooldown`, clamped to one. The half tick is measured: a
/// diamond sword swung twice on the same tick removes 1.4089 health, which is
/// `7 * (0.2 + 0.8 * (0.5/12.5)²)` to four decimals and is not `7 * 0.2`.
[[nodiscard]] f32 attack_strength_scale(i32 ticker, f32 cooldown_ticks) noexcept;

/// The constants the melee rules are made of, each named so a wrong one shows
/// up in a diff rather than hiding inside an expression.
struct CombatConstants {
    /// The floor of the gauge, and the share of the damage the charge scales.
    /// A swing at zero charge is worth a fifth; at full charge, all of it.
    f32 uncharged_share{0.2F};
    f32 charged_share{0.8F};

    /// The half tick the scale is offset by.
    f32 strength_offset{0.5F};

    /// A swing at or above this counts as charged. Everything that needs a
    /// charged swing — the critical, the sweep, the sprint knockback — uses
    /// this one number.
    f32 charged_threshold{0.9F};

    /// What a critical hit multiplies the damage by.
    f32 critical_multiplier{1.5F};

    /// The impulse a plain hit gives the victim, along the horizontal line from
    /// the attacker to it.
    ///
    /// Measured: a bare-handed hit on a sheep three blocks along +x produced
    /// Set Entity Velocity (0.4, 0.4, 0.0) exactly, in blocks per tick.
    f32 base_knockback{0.4F};

    /// The **second** impulse, from Knockback and from sprinting.
    ///
    /// Half a block per tick per level, and a sprint counts as a level. That
    /// second impulse is not along the line between the two: it is along the
    /// **attacker's own facing**, and it halves whatever the first one left.
    ///
    /// That is not a detail, it is the shape of the measurement. A bot at yaw 0
    /// hitting a sheep three blocks along +x while sprinting produced
    /// (0.2, 0.4, 0.5) — the 0.4 along x halved to 0.2, and 0.5 appearing along
    /// **+z**, which is where yaw 0 points and where the sheep was not. A model
    /// that adds the sprint bonus to the same direction gives (0.9, 0.4, 0.0)
    /// and is wrong on two axes out of three.
    f32 knockback_per_level{0.5F};

    /// What a sprinting attacker keeps of its own horizontal velocity after
    /// landing a hit.
    f32 attacker_slowdown{0.6F};

    /// The most upward velocity a knockback can give a victim on the ground.
    f32 knockback_max_up{0.4F};

    /// Damage every entity in the sweep box takes, before Sweeping Edge.
    f32 sweep_base_damage{1.0F};

    /// The knockback a swept entity takes.
    f32 sweep_knockback{0.4F};

    /// Exhaustion charged to the attacker for a hit that connects. The same
    /// number food.hpp holds; repeated here because a caller resolving an
    /// attack should not have to reach into hunger to find it.
    f32 attack_exhaustion{0.1F};

    /// A hit sets the victim's fire for this many ticks per level of Fire
    /// Aspect.
    i32 fire_aspect_ticks_per_level{80};
};

/// The weapon in the attacker's hand, enchantments included.
///
/// Levels rather than ids: this layer does not own an enchantment registry, and
/// what the rules need is how much of each, not which.
struct Weapon {
    /// Registry name, or empty for a bare hand.
    std::string_view item;

    u8 sharpness{0};
    u8 knockback{0};
    u8 sweeping_edge{0};
    u8 fire_aspect{0};
    u8 looting{0};

    /// Not a combat enchantment, and here anyway: every path that resolves an
    /// attack immediately wears the weapon out by it, and a second lookup of
    /// the same stack is a second thing that can disagree with the first.
    u8 unbreaking{0};

    /// ── enchanting ── Smite, Bane of Arthropods and Impaling against *this*
    /// target: +2.5 per level when its mob group is theirs. Scaled by the
    /// charge like Sharpness. Filled by the caller, the only one that knows
    /// what is being hit (see gameplay::damage_bonus).
    f32 target_bonus{0.0F};
};

/// Everything about the attacker that changes the outcome.
///
/// Six of these fields exist only to say when a hit is *not* critical. That
/// asymmetry is the game's: a critical is the ordinary case with six ways to be
/// disqualified, and an implementation that checks three of them makes
/// criticals about twice as common as they should be.
struct AttackerState {
    /// Ticks since the previous attack. Reset to zero by every attack that is
    /// sent, including one that connects with nothing.
    i32 strength_ticker{0};

    bool sprinting{false};
    bool on_ground{true};

    /// Blocks fallen since last on the ground. A critical needs this above
    /// zero, which is why a hit at the top of a jump is not one.
    f32 fall_distance{0.0F};

    bool in_water{false};
    bool on_climbable{false};
    bool blind{false};
    bool riding{false};

    /// Whether the attacker moved further this tick than a walk would.
    /// The sweep needs a standing attacker; a running one gets the sprint
    /// knockback instead.
    bool moving_faster_than_walking{false};

    /// Strength and Weakness, as amplifiers. -1 for neither, 0 for level I.
    i8 strength{-1};
    i8 weakness{-1};
};

/// What one swing came to.
struct AttackOutcome {
    /// True when the swing reached a target at all. False is not an error: a
    /// swing at nothing still resets the gauge and still costs the exhaustion
    /// the game charges for it.
    bool connected{false};

    /// Health to remove from the target.
    f32 damage{0.0F};

    /// The charge the swing was made at, 0..1. Carried out because callers
    /// want it for the particles as well as for the arithmetic.
    f32 strength_scale{0.0F};

    bool critical{false};

    /// The swing swept, and everything else in the box takes `sweep_damage`.
    bool sweeping{false};
    f32  sweep_damage{0.0F};

    /// A charged hit made while sprinting: extra knockback, and the attacker
    /// stops sprinting.
    bool sprint_knockback{false};

    /// The impulse along the line from attacker to victim.
    f32 knockback_strength{0.0F};

    /// The impulse along the attacker's facing, from Knockback and from the
    /// sprint. Zero when there is neither, which is the common case.
    f32 directed_knockback{0.0F};

    /// Exhaustion charged to the attacker.
    f32 exhaustion{0.0F};

    /// Ticks of fire to set on the victim.
    i32 fire_ticks{0};

    /// Durability points the weapon loses. Zero for a bare hand.
    i32 item_damage{0};
};

/// Resolve one swing against one target.
///
/// The order is the game's, and it matters at every step: the charge is taken
/// before the enchantment bonus is scaled by it, the critical is applied to the
/// base damage but not to the bonus, and the sweep is disqualified by the very
/// things — sprinting, the critical — that a charged swing makes likely.
[[nodiscard]] AttackOutcome resolve_attack(const Weapon& weapon, const AttackerState& attacker,
                                           const CombatConstants& constants) noexcept;

/// The share of the swing's damage that Sweeping Edge passes to the sweep.
///
/// `level / (level + 1)`: a third at I, a half at II, three fifths at III. The
/// sweep's own base of one is added on top, so an unenchanted sword sweeps for
/// exactly one point however hard it hit.
[[nodiscard]] f32 sweeping_edge_ratio(u8 level) noexcept;

/// The damage a Sharpness level adds, before the charge scales it.
///
/// `0.5 * level + 0.5`, which is the formula every level of the enchantment
/// follows from I upward.
[[nodiscard]] f32 sharpness_bonus(u8 level) noexcept;

/// A victim's velocity after being knocked back.
///
/// `dx` and `dz` point from the attacker to the victim and need not be
/// normalised — the game normalises them itself, and a zero pair means the two
/// are standing in the same place, in which case nothing happens rather than a
/// NaN spreading through the entity's position.
///
/// The vertical term is the part with a rule of its own: a victim on the ground
/// keeps half its rise plus the impulse, capped; one already in the air keeps
/// what it had, which is why a mob cannot be juggled upward indefinitely.
[[nodiscard]] Vec3d apply_knockback(Vec3d velocity, bool victim_on_ground, f32 strength, f64 dx,
                                    f64 dz, f32 knockback_resistance,
                                    const CombatConstants& constants) noexcept;

/// The horizontal direction a Knockback or sprint impulse travels: the
/// attacker's own facing, as `(sin(yaw), 0, -cos(yaw))`.
///
/// **Known gap.** Java's `Mth.sin` and `Mth.cos` are a table of 65536 floats,
/// not libm, and they differ from `std::sin`/`std::cos` by up to about 5e-5.
/// That table lives in `ov_worldgen`, which is layer 10 and therefore above
/// this one, so what is used here is libm. The error is exact at the four
/// cardinal yaws — which is why the measurement above matches to the last
/// digit — and elsewhere is smaller than one unit of the wire's 1/8000, but it
/// can round a velocity to the neighbouring unit. Moving `mth_sin` down into
/// `ov_math` would close it and is not done here.
[[nodiscard]] Vec3d knockback_direction(f32 yaw_degrees) noexcept;

/// The attacker's own velocity after a sprinting hit.
[[nodiscard]] Vec3d attacker_after_sprint_hit(Vec3d velocity,
                                              const CombatConstants& constants) noexcept;

/// One tick of the attack gauge. Call once per tick, before any attack.
constexpr void tick_attack_strength(AttackerState& attacker) noexcept {
    if (attacker.strength_ticker < 1000) {
        ++attacker.strength_ticker;
    }
}

}  // namespace ov::gameplay
