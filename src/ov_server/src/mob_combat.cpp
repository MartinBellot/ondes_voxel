#define OV_LOG_CATEGORY "server"

#include "mob_combat.hpp"

#include "ov/base/log.hpp"
#include "ov/registry/block_states.hpp"

#include <fstream>
#include <iterator>
#include <utility>

namespace ov::server {

std::optional<gameplay::EntityLootTables> load_entity_loot(
    const std::filesystem::path& pack, const registry::Registries& registries,
    const gameplay::RecipeBook* recipes) {
    std::ifstream stream{pack, std::ios::binary};
    if (!stream) {
        OV_LOG_WARN("no entity loot at {} — a killed mob will drop nothing", pack.string());
        OV_LOG_WARN("run tools/ov_datagen/entity_loot.py to build it");
        return std::nullopt;
    }
    std::vector<u8> bytes{std::istreambuf_iterator<char>{stream},
                          std::istreambuf_iterator<char>{}};

    auto tables = gameplay::EntityLootTables::from_bytes(std::move(bytes), registries, recipes);
    if (!tables) {
        OV_LOG_WARN("cannot read {}: {} — a killed mob will drop nothing", pack.string(),
                    registry::to_string(tables.error()));
        return std::nullopt;
    }
    OV_LOG_INFO("entity loot: {} tables from {}", tables->table_count(), pack.string());
    return std::move(*tables);
}

MobCombat::MobCombat(const registry::Registries&       registries,
                     const gameplay::EntityLootTables* loot) noexcept
    : registries_{&registries},
      loot_{loot},
      entity_registry_{registries.find("minecraft:entity_type")} {}

void MobCombat::tick(const gameplay::DamageConstants& constants) {
    for (auto& [id, window] : windows_) {
        gameplay::tick_health(window, constants);
    }
}

MobHurt MobCombat::hurt(entity::EntityState& state, f32 amount,
                        const gameplay::DamageConstants& constants) {
    auto [row, inserted] = windows_.try_emplace(state.network_id);
    gameplay::HealthState& window = row->second;
    if (inserted) {
        // Seeded from the entity, not from twenty: a zombie has 20 and a cow
        // has 10, and a table that started every mob at the player's maximum
        // would make a cow take two swings too many.
        window.health     = state.health;
        window.max_health = state.max_health;
    }

    // `minecraft:player_attack` is the type, and it is not in
    // #bypasses_invulnerability — which is the whole reason the window applies
    // to a sword at all.
    const gameplay::DamageResult result =
        gameplay::apply_damage(window, gameplay::DamageKind::PlayerAttack, amount, constants);

    state.health = window.health;

    return MobHurt{.applied  = result.applied,
                   .dealt    = result.dealt,
                   .killed   = result.killed,
                   .absorbed = result.absorbed};
}

std::string_view MobCombat::type_name(const entity::EntityState& state) const {
    if (registries_ == nullptr || !entity_registry_) {
        return {};
    }
    return registries_->entry_of(*entity_registry_, state.type);
}

gameplay::DrawResult MobCombat::loot(const entity::EntityState& state, bool killed_by_player,
                                     u8 looting, math::XoroshiroRandomSource& random,
                                     std::vector<gameplay::Drop>& out) const {
    gameplay::DrawResult drawn;
    if (loot_ == nullptr) {
        return drawn;
    }
    const std::string_view name = type_name(state);
    if (name.empty() || !loot_->has_table(name)) {
        // Named rather than silent. A type with no table is a real answer —
        // an armour stand has none — and it is not the same answer as a table
        // that rolled nothing.
        return drawn;
    }

    gameplay::KillContext kill;
    kill.entity_type      = name;
    kill.killed_by_player = killed_by_player;
    kill.looting          = looting;
    // Not tracked by this server, and left at their neutral values rather than
    // guessed: nothing here sets an entity on fire, and `EntityState` carries
    // no slime size — so a slime draws its size-0 row. Both are stated gaps,
    // written down in docs/provenance/branchement.md rather than hidden behind
    // a plausible default.
    kill.on_fire    = false;
    kill.slime_size = 0;

    return loot_->drops(kill, random, out);
}

void MobCombat::forget(i32 network_id) { windows_.erase(network_id); }

}  // namespace ov::server
