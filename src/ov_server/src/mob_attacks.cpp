// ── mobs-3 ── See mob_attacks.hpp.
#define OV_LOG_CATEGORY "server"

#include "mob_attacks.hpp"

#include "survival_session.hpp"  // damage_type_id

#include "ov/base/log.hpp"
#include "ov/gameplay/combat.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/entity.hpp"

#include <algorithm>

namespace ov::server {

WornArmour worn_armour(const registry::Registries&         registries,
                       std::optional<registry::RegistryId> item_registry,
                       std::span<const net::ItemStack>     slots) {
    WornArmour out;
    if (!item_registry) {
        return out;
    }
    for (const net::ItemStack& stack : slots) {
        if (stack.empty() || stack.item_id == 0) {
            continue;
        }
        if (const auto piece =
                gameplay::armour_piece(registries.entry_of(*item_registry, stack.item_id))) {
            out.armour += piece->defense;
            out.toughness += piece->toughness;
            out.knockback_resistance += piece->knockback_resistance;
        }
    }
    return out;
}

MobAttacks::MobAttacks(const registry::Registries& registries, MobCombat* mob_combat)
    : registries_{&registries},
      mob_combat_{mob_combat},
      entity_registry_{registries.find("minecraft:entity_type")} {
    if (entity_registry_) {
        player_type_ = registries.protocol_id(*entity_registry_, "minecraft:player").value_or(-1);
        villager_type_ =
            registries.protocol_id(*entity_registry_, "minecraft:villager").value_or(-1);
    }
    if (const auto attributes = registries.find("minecraft:attribute")) {
        attack_damage_ =
            registries.protocol_id(*attributes, "minecraft:generic.attack_damage").value_or(-1);
    }
    if (player_type_ < 0 || attack_damage_ < 0) {
        OV_LOG_WARN("mob attacks: the registry has no player type or no attack_damage attribute "
                    "— no hostile mob will hurt anyone");
    }
    quarries_.reserve(16);
    attacks_.reserve(64);
}

void MobAttacks::begin_tick() noexcept {
    quarries_.clear();
    attacks_.clear();
}

MobAttackStats MobAttacks::resolve(entity::EntityWorld& world, gameplay::Difficulty difficulty,
                                   i64 game_time, i64 day_time, const MobAttackHost& host,
                                   const AttackDeliver& deliver, std::vector<MobKill>& kills) {
    MobAttackStats stats;
    if (attacks_.empty()) {
        return stats;
    }
    // Inhabited time is not tracked per chunk on this server: its term is 0,
    // which is exact for a young world and low for an old one (named).
    const f32 regional = gameplay::effective_regional_difficulty(
        difficulty, game_time, 0, gameplay::moon_brightness(day_time));

    for (const gameplay::MobAttack& attack : attacks_) {
        ++stats.swings;
        entity::EntityState* attacker = world.mutable_state(attack.attacker);
        if (attacker == nullptr || attacker->removed || attacker->health <= 0.0F) {
            continue;
        }
        const std::optional<f64> base =
            attack_damage_ >= 0 ? world.attribute(attack.attacker, attack_damage_) : std::nullopt;
        if (!base) {
            ++stats.refused;
            continue;
        }
        const f32              damage = static_cast<f32>(*base);
        const std::string_view type =
            entity_registry_ ? registries_->entry_of(*entity_registry_, attacker->type)
                             : std::string_view{};

        // The swing, seen by everyone: Entity Animation 0, the main arm.
        deliver(net::clientbound::kEntityAnimation,
                net::encode_entity_animation(attacker->network_id, 0));

        if (attack.target_is_player) {
            ++stats.on_players;
            const f32 scaled = gameplay::scale_for_difficulty(damage, difficulty);
            if (scaled <= 0.0F || !host.hurt_player ||
                !host.hurt_player(attack.target, scaled, gameplay::DamageKind::MobAttack)) {
                continue;
            }
            ++stats.landed;
            if (host.knock_player) {
                host.knock_player(attack.target, attacker->position);
            }
            if (const auto effect = gameplay::melee_hit_effect(type, difficulty, regional);
                effect && host.give_effect) {
                gameplay::EffectInstance instance;
                instance.effect    = effect->effect;
                instance.duration  = effect->duration;
                instance.amplifier = effect->amplifier;
                host.give_effect(attack.target, instance);
            }
            continue;
        }

        // Another mob — a villager, for the zombie family. No difficulty
        // scaling: the game scales only a hit on a player.
        ++stats.on_mobs;
        const entity::EntityHandle handle = world.find(attack.target);
        entity::EntityState*       victim =
            handle == entity::kNoEntity ? nullptr : world.mutable_state(handle);
        if (victim == nullptr || victim->removed || victim->health <= 0.0F ||
            mob_combat_ == nullptr) {
            continue;
        }
        const MobHurt hurt = mob_combat_->hurt(*victim, damage, damage_constants_);
        if (!hurt.applied) {
            continue;
        }
        ++stats.landed;
        deliver(net::clientbound::kDamageEvent,
                net::encode_damage_event(victim->network_id,
                                         damage_type_id(gameplay::DamageKind::MobAttack),
                                         std::optional<i32>{attacker->network_id},
                                         std::optional<i32>{attacker->network_id}));
        net::MetadataWriter fields;
        fields.float_value(net::metadata::kHealth, victim->health);
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(victim->network_id, fields.take()));
        const gameplay::CombatConstants constants{};
        victim->velocity = gameplay::apply_knockback(
            victim->velocity, victim->on_ground, gameplay::kMobHitKnockback,
            attacker->position.x - victim->position.x, attacker->position.z - victim->position.z,
            0.0F, constants);
        deliver(net::clientbound::kEntityVelocity,
                net::encode_entity_velocity(victim->network_id, victim->velocity.x,
                                            victim->velocity.y, victim->velocity.z));
        if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle))) {
            mob->frighten(60);
        }
        if (!hurt.killed) {
            continue;
        }
        ++stats.kills;
        const std::string_view victim_type =
            entity_registry_ ? registries_->entry_of(*entity_registry_, victim->type)
                             : std::string_view{};
        // The caller tells the death, or replaces the body: a villager that
        // rises as a zombie villager shows no death animation.
        kills.push_back(MobKill{victim->network_id, handle, victim_type, type, victim->position,
                                victim->yaw});
        victim->removed = true;
        mob_combat_->forget(victim->network_id);
    }
    return stats;
}

}  // namespace ov::server
