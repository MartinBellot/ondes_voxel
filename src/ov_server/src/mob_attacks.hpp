// ── mobs-3 ── A hostile mob's swing, carried out on this server.
//
// The rules are ov_gameplay's (mob_attack.hpp): what a hit is worth on each
// difficulty, what armour takes off, which effect a husk or a cave spider
// leaves. The goals only *report* a swing. What is here is the part that knows
// about players and sockets: the list of players a mob may hunt this tick, and
// the hits the entity tick reported, each finished — a player hurt through
// their own survival session and pushed back, a villager hurt and frightened,
// a kill told to the caller (zombification, mobs-3.md § 4).
//
// Callbacks rather than a reference to the server, as for projectiles.hpp.
#pragma once

#include "mob_combat.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/effects.hpp"
#include "ov/gameplay/food.hpp"
#include "ov/gameplay/mob_attack.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::server {

/// Send one packet to every client.
using AttackDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// What the module reaches outside itself for. All three run on the tick
/// thread with the players' lock held.
struct MobAttackHost {
    /// Hurt a player by an amount already scaled for difficulty; the player's
    /// own armour, Resistance and Protection do the rest. True when it landed.
    std::function<bool(i32 player, f32 amount, gameplay::DamageKind kind)> hurt_player;
    /// Push a player away from a point: the caller knows where the player is
    /// and what they wear (knockback resistance), and owns their connection.
    std::function<void(i32 player, Vec3d attacker_feet)> knock_player;
    /// Give a player an effect (a husk's Hunger, a cave spider's Poison).
    std::function<void(i32 player, const gameplay::EffectInstance& effect)> give_effect;
};

/// A mob one of this tick's swings killed. The body is already marked removed
/// and its death told; what it becomes is the caller's (a villager killed by
/// a zombie may rise as a zombie villager).
struct MobKill {
    i32                  victim{0};
    entity::EntityHandle victim_handle{entity::kNoEntity};
    std::string_view     victim_type;
    std::string_view     attacker_type;
    Vec3d                at{};
    f32                  yaw{0.0F};
};

struct MobAttackStats {
    usize swings{0};
    usize on_players{0};
    usize landed{0};
    usize on_mobs{0};
    usize kills{0};
    /// A swing from a mob with no `attack_damage` attribute: counted, not
    /// guessed at.
    usize refused{0};
};

/// The armour a player wears, summed over the four slots from the wiki's
/// table (gameplay::armour_piece).
struct WornArmour {
    f32 armour{0.0F};
    f32 toughness{0.0F};
    f32 knockback_resistance{0.0F};
};
[[nodiscard]] WornArmour worn_armour(const registry::Registries&             registries,
                                     std::optional<registry::RegistryId>     item_registry,
                                     std::span<const net::ItemStack>         slots);

class MobAttacks {
public:
    /// `mob_combat` may be null: then a mob's swing at another mob hurts
    /// nothing.
    MobAttacks(const registry::Registries& registries, MobCombat* mob_combat);

    /// The players hostile mobs may hunt this tick. Refilled by the caller
    /// before the entity tick; empty on Peaceful.
    [[nodiscard]] std::vector<gameplay::Quarry>& quarries() noexcept { return quarries_; }
    /// Where the goals put their swings. Cleared by `begin_tick`.
    [[nodiscard]] std::vector<gameplay::MobAttack>& attacks() noexcept { return attacks_; }

    [[nodiscard]] i32 player_type() const noexcept { return player_type_; }
    [[nodiscard]] i32 villager_type() const noexcept { return villager_type_; }

    /// Before the entity tick: an empty list of swings and of quarries.
    void begin_tick() noexcept;

    /// After the entity tick: every swing, finished. Appends the kills.
    MobAttackStats resolve(entity::EntityWorld& world, gameplay::Difficulty difficulty,
                           i64 game_time, i64 day_time, const MobAttackHost& host,
                           const AttackDeliver& deliver, std::vector<MobKill>& kills);

private:
    const registry::Registries*         registries_{nullptr};
    MobCombat*                          mob_combat_{nullptr};
    std::optional<registry::RegistryId> entity_registry_;
    i32                                 player_type_{-1};
    i32                                 villager_type_{-1};
    i32                                 attack_damage_{-1};
    gameplay::DamageConstants           damage_constants_{};

    std::vector<gameplay::Quarry>    quarries_;
    std::vector<gameplay::MobAttack> attacks_;
    math::XoroshiroRandomSource      loot_random_{0x5EED'0B3AULL, 0x3A77'AC45ULL};
};

}  // namespace ov::server
