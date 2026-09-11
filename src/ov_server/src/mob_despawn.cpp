// ── mobs-3 ── See mob_despawn.hpp.
#include "mob_despawn.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ov::server {

const MobRecord* MobRecords::find(i32 network_id) const noexcept {
    const auto it = records_.find(network_id);
    return it == records_.end() ? nullptr : &it->second;
}

MobDespawn::MobDespawn(const registry::Registries& registries)
    : registries_{&registries}, types_{registries.find("minecraft:entity_type")} {}

DespawnStats MobDespawn::tick(entity::EntityWorld& world, MobRecords& records,
                              std::span<const Vec3d> players, gameplay::Difficulty difficulty,
                              const std::function<bool(i32 type)>& skip) {
    DespawnStats stats;
    if (!types_) {
        return stats;
    }
    for (const entity::EntityHandle handle : world.handles()) {
        entity::EntityState* state = world.mutable_state(handle);
        if (state == nullptr || state->removed || state->health <= 0.0F) {
            continue;
        }
        if (skip && skip(state->type)) {
            continue;
        }
        auto [slot, fresh] = categories_.try_emplace(state->type, gameplay::MobCategory::Misc);
        if (fresh) {
            slot->second = gameplay::category_of(registries_->entry_of(*types_, state->type));
        }
        const gameplay::MobCategory category = slot->second;
        if (category == gameplay::MobCategory::Misc) {
            continue;  // a villager, an item frame: not the spawner's to take back
        }
        // Peaceful takes every monster at once, named or not.
        if (difficulty == gameplay::Difficulty::Peaceful &&
            category == gameplay::MobCategory::Monster) {
            state->removed = true;
            ++stats.peaceful;
            continue;
        }
        // Measured (mobs-3.md § 2): cows 48 and 140 blocks away stayed for
        // three minutes while zombies beside them went — an animal is never
        // removed by distance.
        if (category == gameplay::MobCategory::Creature) {
            continue;
        }
        // A `CustomName` alone does not pin a mob: measured, a zombie summoned
        // with a name at 140 blocks went at once. A name tag pins it by setting
        // `PersistenceRequired`, which is what is read here.
        MobRecord& record = records.at(state->network_id);
        if (record.persistence_required || players.empty()) {
            continue;
        }
        f64 nearest_sq = std::numeric_limits<f64>::max();
        for (const Vec3d& feet : players) {
            const f64 dx = feet.x - state->position.x;
            const f64 dy = feet.y - state->position.y;
            const f64 dz = feet.z - state->position.z;
            nearest_sq   = std::min(nearest_sq, dx * dx + dy * dy + dz * dz);
        }
        const f64                      distance = std::sqrt(nearest_sq);
        const gameplay::CategoryRules rules    = gameplay::rules_for(category);
        ++record.no_action_ticks;
        if (distance < static_cast<f64>(rules.no_despawn_distance)) {
            record.no_action_ticks = 0;
        }
        switch (gameplay::decide_despawn(category, distance, false, record.no_action_ticks,
                                         random_)) {
            case gameplay::DespawnDecision::Keep:
                break;
            case gameplay::DespawnDecision::Immediate:
                state->removed = true;
                ++stats.immediate;
                break;
            case gameplay::DespawnDecision::Random:
                state->removed = true;
                ++stats.random;
                break;
        }
    }
    return stats;
}

}  // namespace ov::server
