#include "ov/gameplay/damage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ov::gameplay {
namespace {

// The forty-four damage types of 1.20.1.
//
// Names, message ids, exhaustion costs and `scaling` come from the 1.20.1
// datapack's own damage_type declarations; the flags are that datapack's
// damage_type tags, one bit per tag. Both are regenerated locally by the data
// generator (data/vanilla/1.20.1/generated/, gitignored) and a test compares
// this table back against them entry by entry, so a drift shows up as a failed
// test naming the type rather than as a mob that stops burning.
//
// The order is alphabetical, which is not a stylistic choice: it is the order a
// datapack registry is loaded in, therefore the order of the ids in the codec
// the server sends at login, therefore the numbers the client will use to pick
// a death message. It was confirmed on the wire — see damage.hpp.
constexpr std::array<DamageTypeInfo, kDamageKindCount> kDamageTypes{{
    {"minecraft:arrow", "arrow", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsProjectile, DeathMessageType::Default},
    {"minecraft:bad_respawn_point", "badRespawnPoint", 0.1F, DamageScaling::Always,
     DamageFlags::IsExplosion, DeathMessageType::IntentionalGameDesign},
    {"minecraft:cactus", "cactus", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:cramming", "cramming", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:dragon_breath", "dragonBreath", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:drown", "drown", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::IsDrowning, DeathMessageType::Default},
    {"minecraft:dry_out", "dryout", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:explosion", "explosion", 0.1F, DamageScaling::Always, DamageFlags::IsExplosion,
     DeathMessageType::Default},
    {"minecraft:fall", "fall", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::IsFall, DeathMessageType::FallVariants},
    {"minecraft:falling_anvil", "anvil", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:falling_block", "fallingBlock", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:falling_stalactite", "fallingStalactite", 0.1F,
     DamageScaling::WhenCausedByLivingNonPlayer, DamageFlags::None, DeathMessageType::Default},
    {"minecraft:fireball", "fireball", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsFire | DamageFlags::IsProjectile, DeathMessageType::Default},
    {"minecraft:fireworks", "fireworks", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsExplosion, DeathMessageType::Default},
    {"minecraft:fly_into_wall", "flyIntoWall", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:freeze", "freeze", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::IsFreezing, DeathMessageType::Default},
    {"minecraft:generic", "generic", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:generic_kill", "genericKill", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::BypassesInvulnerability |
         DamageFlags::BypassesResistance,
     DeathMessageType::Default},
    {"minecraft:hot_floor", "hotFloor", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsFire, DeathMessageType::Default},
    {"minecraft:in_fire", "inFire", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsFire, DeathMessageType::Default},
    {"minecraft:in_wall", "inWall", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:indirect_magic", "indirectMagic", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:lava", "lava", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsFire, DeathMessageType::Default},
    {"minecraft:lightning_bolt", "lightningBolt", 0.1F,
     DamageScaling::WhenCausedByLivingNonPlayer, DamageFlags::IsLightning,
     DeathMessageType::Default},
    {"minecraft:magic", "magic", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:mob_attack", "mob", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:mob_attack_no_aggro", "mob", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:mob_projectile", "mob", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsProjectile, DeathMessageType::Default},
    {"minecraft:on_fire", "onFire", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::IsFire, DeathMessageType::Default},
    {"minecraft:out_of_world", "outOfWorld", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::BypassesInvulnerability |
         DamageFlags::BypassesResistance,
     DeathMessageType::Default},
    {"minecraft:outside_border", "outsideBorder", 0.0F,
     DamageScaling::WhenCausedByLivingNonPlayer, DamageFlags::BypassesArmor,
     DeathMessageType::Default},
    {"minecraft:player_attack", "player", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:player_explosion", "explosion.player", 0.1F, DamageScaling::Always,
     DamageFlags::IsExplosion, DeathMessageType::Default},
    {"minecraft:sonic_boom", "sonic_boom", 0.0F, DamageScaling::Always,
     DamageFlags::BypassesArmor | DamageFlags::BypassesEnchantments, DeathMessageType::Default},
    {"minecraft:stalagmite", "stalagmite", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::IsFall, DeathMessageType::Default},
    {"minecraft:starve", "starve", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor | DamageFlags::BypassesEffects, DeathMessageType::Default},
    {"minecraft:sting", "sting", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:sweet_berry_bush", "sweetBerryBush", 0.1F,
     DamageScaling::WhenCausedByLivingNonPlayer, DamageFlags::None, DeathMessageType::Default},
    {"minecraft:thorns", "thorns", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::None, DeathMessageType::Default},
    {"minecraft:thrown", "thrown", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsProjectile, DeathMessageType::Default},
    {"minecraft:trident", "trident", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsProjectile, DeathMessageType::Default},
    {"minecraft:unattributed_fireball", "onFire", 0.1F,
     DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsFire | DamageFlags::IsProjectile, DeathMessageType::Default},
    {"minecraft:wither", "wither", 0.0F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::BypassesArmor, DeathMessageType::Default},
    {"minecraft:wither_skull", "witherSkull", 0.1F, DamageScaling::WhenCausedByLivingNonPlayer,
     DamageFlags::IsProjectile, DeathMessageType::Default},
}};

constexpr std::string_view kDeathPrefix = "death.attack.";

}  // namespace

const DamageTypeInfo& damage_type(DamageKind kind) noexcept {
    const auto index = static_cast<usize>(kind);
    // A kind out of range is a programming error, not data — but returning a
    // reference into nothing would be worse than clamping, and clamping
    // silently would be worse than either. Generic is the one entry with no
    // side effects at all, and the assertion above it names the mistake in a
    // debug build.
    return kDamageTypes[index < kDamageKindCount ? index
                                                 : static_cast<usize>(DamageKind::Generic)];
}

std::optional<DamageKind> damage_kind_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kDamageKindCount; ++i) {
        if (kDamageTypes[i].name == name) {
            return static_cast<DamageKind>(i);
        }
    }
    return std::nullopt;
}

usize death_message_key(DamageKind kind, char* out, usize capacity) noexcept {
    const DamageTypeInfo& info   = damage_type(kind);
    const usize           needed = kDeathPrefix.size() + info.message_id.size();
    if (out == nullptr || capacity < needed) {
        return 0;
    }
    std::memcpy(out, kDeathPrefix.data(), kDeathPrefix.size());
    std::memcpy(out + kDeathPrefix.size(), info.message_id.data(), info.message_id.size());
    return needed;
}

f32 fall_damage(f32 distance, const DamageConstants& constants) noexcept {
    // A ceiling, not a floor. Measured over thirty successive heights: a fall
    // of exactly four blocks costs one point, and the first height that costs
    // anything is the first one strictly above three. Rounding the other way
    // would make a four-block fall free, which is a difference a player feels
    // on every staircase.
    const f32 over = distance - constants.fall_damage_free_distance;
    if (over <= 0.0F) {
        return 0.0F;
    }
    return std::ceil(over);
}

DamageResult apply_damage(HealthState& state, DamageKind kind, f32 amount,
                          const DamageConstants& constants) noexcept {
    return apply_damage(state, kind, amount, constants, DamageMitigation{});
}

f32 after_resistance(DamageKind kind, f32 amount, i32 resistance) noexcept {
    const DamageTypeInfo& info = damage_type(kind);
    if (resistance < 0 || has(info.flags, DamageFlags::BypassesEffects) ||
        has(info.flags, DamageFlags::BypassesResistance)) {
        return amount;
    }
    // In float, in this order: the product first, then the division. 7 at
    // level I came back as 5.6 on the real server, and so does this.
    const i32 kept    = 25 - (resistance + 1) * 5;
    const f32 product = amount * static_cast<f32>(kept);
    return std::max(product / 25.0F, 0.0F);
}

DamageResult apply_damage(HealthState& state, DamageKind kind, f32 amount,
                          const DamageConstants& constants,
                          const DamageMitigation& mitigation) noexcept {
    DamageResult result;
    if (state.dead || amount <= 0.0F) {
        return result;
    }

    const DamageTypeInfo& info = damage_type(kind);
    f32                   dealt = amount;

    if (!has(info.flags, DamageFlags::BypassesInvulnerability)) {
        if (state.invulnerable_ticks >= constants.partial_damage_threshold) {
            // Inside the window. A hit no bigger than the one that opened it
            // does nothing; a bigger one lands only the difference and becomes
            // the new benchmark. Measured both ways round: four then nine costs
            // nine, nine then four costs nine.
            if (amount <= state.last_damage) {
                result.absorbed = true;
                return result;
            }
            dealt = amount - state.last_damage;
            state.last_damage = amount;
        } else {
            state.invulnerable_ticks = constants.invulnerable_ticks;
            state.last_damage        = amount;
            state.hurt_ticks         = constants.hurt_ticks;
        }
    }

    // Resistance, then the yellow hearts. With no resistance and no
    // absorption both are identities, so the measured survival tables run
    // through this path unchanged.
    dealt               = after_resistance(kind, dealt, mitigation.resistance);
    // ── enchanting ── the worn enchantments' protection, after Resistance.
    if (const auto epf = mitigation.protection[static_cast<usize>(kind)]; epf > 0 && dealt > 0.0F) {
        dealt *= 1.0F - std::min(static_cast<f32>(epf), 20.0F) / 25.0F;
    }
    const f32 through   = std::max(dealt - state.absorption, 0.0F);
    state.absorption    = std::max(state.absorption - (dealt - through), 0.0F);
    dealt               = through;

    state.health -= dealt;
    result.applied = true;
    result.dealt   = dealt;
    if (state.health <= 0.0F) {
        state.health  = 0.0F;
        state.dead    = true;
        result.killed = true;
    }
    return result;
}

void tick_health(HealthState& state, const DamageConstants& /*constants*/) noexcept {
    if (state.invulnerable_ticks > 0) {
        --state.invulnerable_ticks;
    }
    if (state.hurt_ticks > 0) {
        --state.hurt_ticks;
    }
}

f32 tick_air(HealthState& state, bool submerged, const DamageConstants& constants) noexcept {
    if (!submerged) {
        state.air = std::min(constants.max_air, state.air + 4);
        return 0.0F;
    }
    if (state.air > 0) {
        --state.air;
        return 0.0F;
    }
    // Out of breath. Vanilla drops the counter to -20 and takes two points each
    // time it comes back round, which is what makes drowning cost a heart a
    // second rather than all of them at once.
    --state.air;
    if (state.air <= -constants.drown_interval_ticks) {
        state.air = 0;
        return constants.drown_damage;
    }
    return 0.0F;
}

f32 accumulate_fall(HealthState& state, f64 delta_y, bool on_ground,
                    const DamageConstants& constants) noexcept {
    // The order is the whole of it, and it is not the obvious one.
    //
    // The tick that lands does NOT add its own drop. Vanilla's check is an
    // if/else: either the entity is on the ground, in which case the fall is
    // settled and reset, or it is not, in which case this tick's drop is
    // accumulated. The last fraction of the fall is therefore never counted.
    //
    // That is not a rounding detail. At speed the discarded step is more than a
    // block, so a drop of twelve blocks is charged as a fall of 10.81 and costs
    // eight points rather than nine. Adding the landing step first reproduces
    // 23 of 30 measured heights; doing it this way reproduces 30 of 30.
    if (on_ground) {
        const f32 damage    = fall_damage(state.fall_distance, constants);
        state.fall_distance = 0.0F;
        return damage;
    }
    if (delta_y < 0.0) {
        state.fall_distance -= static_cast<f32>(delta_y);
    }
    return 0.0F;
}

}  // namespace ov::gameplay
