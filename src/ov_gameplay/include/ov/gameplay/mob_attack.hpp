// ── mobs-3 ── A hostile mob's hit, as the rules see it.
//
// Until this wave a zombie on this server chased nothing: the target goal
// searched the *entity world*, and players are not in it — they live in the
// server's player table. `MeleeAttackGoal` counted down its cooldown and no hit
// ever came of it (docs/provenance/mobs-2.md § 6.1). Two things were missing:
//
//   * **the players, as quarry.** The caller lists the players a hostile mob may
//     hunt this tick — connected, alive, in survival or adventure — through
//     `MobContext::quarries`, the way it already lists the players an animal
//     may be tempted by. The target goal searches that list for a player type.
//
//   * **the hit.** The goal does not apply damage: ov_gameplay knows what a hit
//     *is* but not who is allowed to be told about it. It appends a `MobAttack`
//     for the caller, which scales it by difficulty, runs it through the
//     player's armour, Resistance and Protection, pushes the player back, and
//     gives the effect a husk or a cave spider leaves.
//
// Every number below is either measured against a real 1.20.1 server by
// scripts/measure_mobs3.py or stated as documentary (docs/provenance/mobs-3.md).
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/effects.hpp"
#include "ov/gameplay/food.hpp"
#include "ov/gameplay/spawn_rules.hpp"  // moon_brightness
#include "ov/math/vec.hpp"

#include <optional>
#include <string_view>

namespace ov::gameplay {

/// A player a hostile mob may hunt, as the mob sees one.
///
/// Only the players that can be hunted at all are listed: a creative or
/// spectator player, a dead one, or any player on Peaceful is left out by the
/// caller, which is what makes a target dropped the tick its player changes
/// game mode.
struct Quarry {
    i32   network_id{0};
    /// The protocol id of `minecraft:player`, matched against the goal's type.
    i32   type{-1};
    Vec3d feet{};
    f32   width{0.6F};
    f32   height{1.8F};
    f32   eye_height{1.62F};
};

/// A target goal's type meaning "the villager type, whatever its id": the
/// goal list is built without the registry, so the caller supplies the id at
/// tick time (`MobContext::villager_type`). A zombie hunts villagers **without**
/// line of sight — mobs.md § 0.
inline constexpr i32 kVillagerQuarry = -3;

/// A swing that landed on this tick, for the caller to finish.
struct MobAttack {
    entity::EntityHandle attacker{entity::kNoEntity};
    /// The target's wire id: a player's, or a mob's.
    i32  target{0};
    bool target_is_player{false};
};

/// The melee goal's cooldown between two hits. Measured: 20 ticks on every
/// difficulty (mobs-3.md § 1).
inline constexpr i32 kMeleeCooldownTicks = 20;

/// The squared reach of a melee attack: `(2·w)² + w_target`, in float as the
/// game computes it — `w·2·w·2 + w_target`, left to right, then widened. A
/// zombie reaches a player at √2.04 ≈ 1.43 blocks, feet to feet.
[[nodiscard]] constexpr f64 melee_reach_sq(f32 attacker_width, f32 target_width) noexcept {
    const f32 product = attacker_width * 2.0F * attacker_width * 2.0F + target_width;
    return static_cast<f64>(product);
}

/// The strength of the push every hit gives. The same 0.4 as a player's
/// punch (combat.md); a mob's `attack_knockback` attribute adds to it, and is
/// 0 for every species here.
inline constexpr f32 kMobHitKnockback = 0.4F;

/// A hit on a player, scaled by difficulty — for the damage types whose
/// `scaling` says so (`when_caused_by_living_non_player` for a mob's melee and
/// its arrows, `always` for an explosion): nothing on Peaceful,
/// `min(a / 2 + 1, a)` on Easy, `a` on Normal, `1.5·a` on Hard.
[[nodiscard]] f32 scale_for_difficulty(f32 amount, Difficulty difficulty) noexcept;

// The moon's brightness for a day time is spawn_rules.hpp's `moon_brightness`
// (mobs-2), included above: the regional difficulty below reads the same moon.

/// The effective regional difficulty at a position (the wiki's *Difficulty*):
///
///   base 0.75, plus a quarter of the world's age past its first hour
///   (clamped over 20 hours), plus a term from how long the chunk has been
///   inhabited and one from the moon (halved on Easy), times the difficulty's
///   id. 0 on Peaceful.
///
/// A fresh world reads `0.75 · id`: 0.75, 1.5, 2.25.
[[nodiscard]] f32 effective_regional_difficulty(Difficulty difficulty, i64 game_time,
                                                i64 inhabited_time, f32 moon) noexcept;

/// An effect a melee hit leaves on its target.
struct HitEffect {
    Effect effect{Effect::Speed};
    i32    duration{0};
    u8     amplifier{0};
};

/// The effect a species' melee leaves, or nothing.
///
///   husk          Hunger, 140 ticks × ⌊regional difficulty⌋ — none on Easy
///                 while the world is young, since ⌊0.75⌋ is 0
///   cave spider   Poison, 7 s on Normal, 15 s on Hard, none on Easy
///   wither sk.    Wither, 10 s (no brain on this server: the table is ready)
[[nodiscard]] std::optional<HitEffect> melee_hit_effect(std::string_view attacker_type,
                                                        Difficulty difficulty,
                                                        f32 regional) noexcept;

/// One worn piece's armour, from the wiki's *Armor* table. Nothing for an item
/// that is not armour — refused, never zero-filled.
struct ArmourPiece {
    f32 defense{0.0F};
    f32 toughness{0.0F};
    f32 knockback_resistance{0.0F};
};
[[nodiscard]] std::optional<ArmourPiece> armour_piece(std::string_view item) noexcept;

}  // namespace ov::gameplay
