#include "ov/gameplay/combat.hpp"

#include "ov/gameplay/durability.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

/// The measured table.
///
/// Every row is two numbers read off `/attribute get` on a player holding the
/// item, and every row was then confirmed by a fully charged hit into a sheep
/// whose health was read back. The two agree exactly on all thirty-four.
///
/// Three things in it are worth stating because none is guessable from the
/// pattern of the others:
///
///   * Gold is not a tier of its own here. A golden sword hits for four, the
///     same as wood, and a golden hoe swings at 1.0, the same as wood — while
///     mining, gold is the *fastest* tier. Two orderings, one material.
///   * Axes are slower than swords and hit harder, and their speeds are not
///     one series: wood and stone share 0.8, iron takes 0.9, and diamond and
///     netherite share 1.0.
///   * Hoes go the other way. Their damage is the bare hand's 1.0 at every
///     tier, and only their speed climbs — 1.0, 2.0, 3.0, 4.0, 4.0.
constexpr WeaponStats kWeapons[] = {
    {"minecraft:wooden_sword", 4.0F, 1.6F},      {"minecraft:stone_sword", 5.0F, 1.6F},
    {"minecraft:iron_sword", 6.0F, 1.6F},        {"minecraft:golden_sword", 4.0F, 1.6F},
    {"minecraft:diamond_sword", 7.0F, 1.6F},     {"minecraft:netherite_sword", 8.0F, 1.6F},

    {"minecraft:wooden_axe", 7.0F, 0.8F},        {"minecraft:stone_axe", 9.0F, 0.8F},
    {"minecraft:iron_axe", 9.0F, 0.9F},          {"minecraft:golden_axe", 7.0F, 1.0F},
    {"minecraft:diamond_axe", 9.0F, 1.0F},       {"minecraft:netherite_axe", 10.0F, 1.0F},

    {"minecraft:wooden_pickaxe", 2.0F, 1.2F},    {"minecraft:stone_pickaxe", 3.0F, 1.2F},
    {"minecraft:iron_pickaxe", 4.0F, 1.2F},      {"minecraft:golden_pickaxe", 2.0F, 1.2F},
    {"minecraft:diamond_pickaxe", 5.0F, 1.2F},   {"minecraft:netherite_pickaxe", 6.0F, 1.2F},

    {"minecraft:wooden_shovel", 2.5F, 1.0F},     {"minecraft:stone_shovel", 3.5F, 1.0F},
    {"minecraft:iron_shovel", 4.5F, 1.0F},       {"minecraft:golden_shovel", 2.5F, 1.0F},
    {"minecraft:diamond_shovel", 5.5F, 1.0F},    {"minecraft:netherite_shovel", 6.5F, 1.0F},

    {"minecraft:wooden_hoe", 1.0F, 1.0F},        {"minecraft:stone_hoe", 1.0F, 2.0F},
    {"minecraft:iron_hoe", 1.0F, 3.0F},          {"minecraft:golden_hoe", 1.0F, 1.0F},
    {"minecraft:diamond_hoe", 1.0F, 4.0F},       {"minecraft:netherite_hoe", 1.0F, 4.0F},

    {"minecraft:trident", 9.0F, 1.1F},
};

/// A player holding nothing in particular. Measured on `air`, `stick` and
/// `shears`, which all answer the same.
constexpr WeaponStats kBareHand{"", 1.0F, 4.0F};

/// The three ways to hold a sword, for the sweep's own condition.
[[nodiscard]] bool is_sword(std::string_view item) noexcept {
    return item.size() > 6 && item.substr(item.size() - 6) == "_sword";
}

/// Strength and Weakness, as flat terms.
///
/// Both are attribute modifiers on `generic.attack_damage` and both are
/// additive: +3 per level of Strength, -4 per level of Weakness. They act on
/// the base damage before the charge scales it, which is why a weak player
/// spamming clicks can do no damage at all.
[[nodiscard]] f32 effect_bonus(i8 strength, i8 weakness) noexcept {
    f32 bonus = 0.0F;
    if (strength >= 0) {
        bonus += 3.0F * static_cast<f32>(strength + 1);
    }
    if (weakness >= 0) {
        bonus -= 4.0F * static_cast<f32>(weakness + 1);
    }
    return bonus;
}

}  // namespace

std::span<const WeaponStats> weapon_table() noexcept { return kWeapons; }

WeaponStats weapon_for(std::string_view item) noexcept {
    for (const WeaponStats& stats : kWeapons) {
        if (stats.item == item) {
            return stats;
        }
    }
    return WeaponStats{item, kBareHand.attack_damage, kBareHand.attack_speed};
}

f32 attack_cooldown_ticks(f32 attack_speed) noexcept {
    // A speed of zero would be a weapon that never recharges. It does not occur
    // — the smallest in the game is 0.8 — and dividing by it would hand every
    // caller an infinity, so it is answered with one tick: the gauge is then
    // always full, which is the honest reading of "no cooldown".
    if (attack_speed <= 0.0F) {
        return 1.0F;
    }
    return 20.0F / attack_speed;
}

f32 attack_strength_scale(i32 ticker, f32 cooldown_ticks) noexcept {
    if (cooldown_ticks <= 0.0F) {
        return 1.0F;
    }
    const f32 raw = (static_cast<f32>(ticker) + 0.5F) / cooldown_ticks;
    return std::clamp(raw, 0.0F, 1.0F);
}

f32 sharpness_bonus(u8 level) noexcept {
    if (level == 0) {
        return 0.0F;
    }
    return 0.5F * static_cast<f32>(level) + 0.5F;
}

f32 sweeping_edge_ratio(u8 level) noexcept {
    if (level == 0) {
        return 0.0F;
    }
    // level / (level + 1): a third at I, a half at II, three fifths at III.
    return static_cast<f32>(level) / static_cast<f32>(level + 1);
}

AttackOutcome resolve_attack(const Weapon& weapon, const AttackerState& attacker,
                             const CombatConstants& constants) noexcept {
    const WeaponStats stats    = weapon_for(weapon.item);
    const f32         cooldown = attack_cooldown_ticks(stats.attack_speed);
    const f32         scale    = attack_strength_scale(attacker.strength_ticker, cooldown);

    AttackOutcome out;
    out.connected      = true;
    out.strength_scale = scale;
    out.exhaustion     = constants.attack_exhaustion;
    // Delegated rather than repeated. The rule — a sword costs one, every other
    // tool two, shears nothing — is measured in durability.cpp, and a second
    // copy here would be a second thing that can disagree with the first. It
    // already did: this file's own version charged shears two.
    out.item_damage = action_damage(weapon.item, ToolAction::Attack);

    f32 base  = stats.attack_damage + effect_bonus(attacker.strength, attacker.weakness);
    f32 bonus = sharpness_bonus(weapon.sharpness);

    // The charge scales both, but not the same way: the weapon's damage keeps a
    // fifth of itself at zero charge, the enchantment keeps nothing. A Sharpness
    // V sword spammed does the damage of an unenchanted one.
    base *= constants.uncharged_share + scale * scale * constants.charged_share;
    bonus *= scale;

    const bool charged = scale > constants.charged_threshold;

    out.critical = charged && attacker.fall_distance > 0.0F && !attacker.on_ground &&
                   !attacker.on_climbable && !attacker.in_water && !attacker.blind &&
                   !attacker.riding && !attacker.sprinting;
    if (out.critical) {
        // The multiplier is on the weapon's damage only. The enchantment bonus
        // is added afterwards and is never multiplied, which is why Sharpness
        // is worth relatively less on a critical than off one.
        base *= constants.critical_multiplier;
    }

    if (base < 0.0F) {
        // Weakness can take a bare hand below nothing. The game floors the hit
        // at zero rather than healing the target.
        base = 0.0F;
    }
    out.damage = base + bonus;

    out.sprint_knockback = charged && attacker.sprinting;

    // The sweep is what is left after everything else has claimed the swing:
    // charged, not a critical, not a sprinting hit, standing on the ground,
    // barely moving, and holding a sword. Six conditions, and dropping any one
    // of them makes the sweep fire on hits the game keeps plain.
    out.sweeping = charged && !out.critical && !out.sprint_knockback && attacker.on_ground &&
                   !attacker.moving_faster_than_walking && is_sword(weapon.item);
    if (out.sweeping) {
        out.sweep_damage =
            constants.sweep_base_damage +
            sweeping_edge_ratio(weapon.sweeping_edge) * out.damage;
    }

    // Two impulses, not one sum. The first is the hit's own and points away
    // from the attacker; the second is Knockback's and the sprint's, and points
    // wherever the attacker is facing.
    out.knockback_strength = constants.base_knockback;
    f32 knockback_levels   = static_cast<f32>(weapon.knockback);
    if (out.sprint_knockback) {
        knockback_levels += 1.0F;
    }
    out.directed_knockback = knockback_levels * constants.knockback_per_level;

    if (weapon.fire_aspect > 0) {
        out.fire_ticks =
            constants.fire_aspect_ticks_per_level * static_cast<i32>(weapon.fire_aspect);
    }
    return out;
}

Vec3d apply_knockback(Vec3d velocity, bool victim_on_ground, f32 strength, f64 dx, f64 dz,
                      f32 knockback_resistance, const CombatConstants& constants) noexcept {
    const f32 effective = strength * (1.0F - knockback_resistance);
    if (effective <= 0.0F) {
        return velocity;
    }
    const f64 length = std::sqrt(dx * dx + dz * dz);
    if (length < 1.0E-7) {
        // The attacker and the victim are in the same place. Nothing rather
        // than a division that makes the victim's position NaN forever.
        return velocity;
    }
    const f64 nx = dx / length;
    const f64 nz = dz / length;
    const f64 impulse = static_cast<f64>(effective);

    Vec3d out;
    out.x = velocity.x / 2.0 - nx * impulse;
    out.z = velocity.z / 2.0 - nz * impulse;
    out.y = victim_on_ground
                ? std::min(static_cast<f64>(constants.knockback_max_up), velocity.y / 2.0 + impulse)
                : velocity.y;
    return out;
}

Vec3d knockback_direction(f32 yaw_degrees) noexcept {
    const f64 radians = static_cast<f64>(yaw_degrees) * 3.14159265358979323846 / 180.0;
    return Vec3d{std::sin(radians), 0.0, -std::cos(radians)};
}

Vec3d attacker_after_sprint_hit(Vec3d velocity, const CombatConstants& constants) noexcept {
    const f64 keep = static_cast<f64>(constants.attacker_slowdown);
    return Vec3d{velocity.x * keep, velocity.y, velocity.z * keep};
}

}  // namespace ov::gameplay
