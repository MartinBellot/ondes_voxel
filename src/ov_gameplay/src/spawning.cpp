#include "ov/gameplay/spawning.hpp"

#include "ov/gameplay/pathfinding.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {
namespace {

/// The chunks a 17x17 spawn square holds. The cap is expressed against this
/// number, so a server ticking fewer chunks holds proportionally fewer mobs.
constexpr i32 kSpawnSquareChunks = 17 * 17;

struct CategoryName {
    std::string_view name;
    MobCategory      category;
};

/// Which category each type spawns in.
///
/// A table, because nothing measured about an entity type says which one it
/// belongs to. Only the types this milestone actually spawns are listed plus
/// the handful whose category a caller is likely to ask for; everything else is
/// `Misc`, which never spawns naturally.
constexpr CategoryName kCategories[] = {
    {"minecraft:zombie", MobCategory::Monster},
    {"minecraft:zombie_villager", MobCategory::Monster},
    {"minecraft:husk", MobCategory::Monster},
    {"minecraft:drowned", MobCategory::Monster},
    {"minecraft:skeleton", MobCategory::Monster},
    {"minecraft:stray", MobCategory::Monster},
    {"minecraft:creeper", MobCategory::Monster},
    {"minecraft:spider", MobCategory::Monster},
    {"minecraft:cave_spider", MobCategory::Monster},
    {"minecraft:enderman", MobCategory::Monster},
    {"minecraft:witch", MobCategory::Monster},
    {"minecraft:slime", MobCategory::Monster},
    {"minecraft:silverfish", MobCategory::Monster},
    {"minecraft:phantom", MobCategory::Monster},

    {"minecraft:cow", MobCategory::Creature},
    {"minecraft:pig", MobCategory::Creature},
    {"minecraft:sheep", MobCategory::Creature},
    {"minecraft:chicken", MobCategory::Creature},
    {"minecraft:horse", MobCategory::Creature},
    {"minecraft:donkey", MobCategory::Creature},
    {"minecraft:rabbit", MobCategory::Creature},
    {"minecraft:wolf", MobCategory::Creature},
    {"minecraft:llama", MobCategory::Creature},
    {"minecraft:fox", MobCategory::Creature},
    {"minecraft:goat", MobCategory::Creature},

    {"minecraft:bat", MobCategory::Ambient},

    {"minecraft:squid", MobCategory::WaterCreature},
    {"minecraft:dolphin", MobCategory::WaterCreature},

    {"minecraft:glow_squid", MobCategory::UndergroundWaterCreature},

    {"minecraft:cod", MobCategory::WaterAmbient},
    {"minecraft:salmon", MobCategory::WaterAmbient},
    {"minecraft:tropical_fish", MobCategory::WaterAmbient},
    {"minecraft:pufferfish", MobCategory::WaterAmbient},

    {"minecraft:axolotl", MobCategory::Axolotls},
};

[[nodiscard]] usize category_index(MobCategory category) noexcept {
    return static_cast<usize>(category);
}

[[nodiscard]] bool is_water(const world::LevelView& level, BlockPos pos) {
    const registry::BlockRegistry& blocks = level.blocks();
    const registry::BlockStateId   state  = level.block_at(pos);
    if (!blocks.holds_fluid(state)) {
        return false;
    }
    return blocks.block_name(blocks.block_of(state)) != "minecraft:lava";
}

}  // namespace

std::string_view to_string(MobCategory category) noexcept {
    switch (category) {
        case MobCategory::Monster: return "monster";
        case MobCategory::Creature: return "creature";
        case MobCategory::Ambient: return "ambient";
        case MobCategory::Axolotls: return "axolotls";
        case MobCategory::UndergroundWaterCreature: return "underground_water_creature";
        case MobCategory::WaterCreature: return "water_creature";
        case MobCategory::WaterAmbient: return "water_ambient";
        case MobCategory::Misc: return "misc";
    }
    return "?";
}

CategoryRules rules_for(MobCategory category) noexcept {
    CategoryRules rules;
    switch (category) {
        case MobCategory::Monster:
            rules.cap = 70;
            rules.despawn_distance = 128;
            rules.max_spawn_light = 0;
            break;
        case MobCategory::Creature:
            rules.cap = 10;
            rules.despawn_distance = 128;
            // Animals do not test light. What they test — a grass block, and
            // sky above — is a different rule and lives in `can_spawn_at`.
            rules.max_spawn_light = -1;
            break;
        case MobCategory::Ambient:
            rules.cap = 15;
            rules.despawn_distance = 128;
            rules.max_spawn_light = 3;
            break;
        case MobCategory::Axolotls:
            rules.cap = 5;
            rules.despawn_distance = 128;
            rules.aquatic = true;
            break;
        case MobCategory::UndergroundWaterCreature:
            rules.cap = 5;
            rules.despawn_distance = 128;
            rules.aquatic = true;
            break;
        case MobCategory::WaterCreature:
            rules.cap = 5;
            rules.despawn_distance = 128;
            rules.aquatic = true;
            break;
        case MobCategory::WaterAmbient:
            rules.cap = 20;
            rules.despawn_distance = 64;
            rules.aquatic = true;
            break;
        case MobCategory::Misc:
            rules.cap = 0;
            // Never removed by distance: an item on the ground, a boat, a
            // painting. Zero here would delete every dropped stack the moment
            // a player walked away.
            rules.despawn_distance = 0;
            break;
    }
    return rules;
}

MobCategory category_of(std::string_view type_name) noexcept {
    for (const CategoryName& entry : kCategories) {
        if (entry.name == type_name) {
            return entry.category;
        }
    }
    return MobCategory::Misc;
}

NaturalSpawner::NaturalSpawner(u64 seed) : random_{static_cast<i64>(seed)} {
    scratch_.reserve(64);
    for (std::vector<SpawnerEntry>& list : entries_) {
        list.reserve(16);
    }
}

void NaturalSpawner::set_entries(MobCategory category, std::span<const SpawnerEntry> entries) {
    std::vector<SpawnerEntry>& list = entries_[category_index(category)];
    list.assign(entries.begin(), entries.end());
}

const std::vector<SpawnerEntry>& NaturalSpawner::entries(MobCategory category) const noexcept {
    return entries_[category_index(category)];
}

i32 NaturalSpawner::effective_cap(MobCategory category, usize eligible_chunks) noexcept {
    const i32 cap = rules_for(category).cap;
    if (cap == 0) {
        return 0;
    }
    // Integer maths, in this order. Dividing first would round every small
    // server's cap to zero, and a server ticking 100 chunks would hold no mobs
    // at all — which is a bug that looks exactly like "spawning is broken".
    const i64 scaled = static_cast<i64>(cap) * static_cast<i64>(eligible_chunks) /
                       static_cast<i64>(kSpawnSquareChunks);
    return static_cast<i32>(scaled);
}

bool NaturalSpawner::can_spawn_at(const SpawnEnvironment& environment, MobCategory category,
                                  BlockPos pos, f32 width, f32 height) const {
    if (environment.level == nullptr) {
        return false;
    }
    const world::LevelView& level = *environment.level;
    const CategoryRules     rules = rules_for(category);

    if (!level.is_loaded(pos)) {
        // Not "no": *unknown*. Spawning into a chunk that is not loaded would
        // make the world depend on which chunks happened to be resident, which
        // is the same class of bug as a fluid flowing into one.
        return false;
    }
    const world::WorldShape shape = level.shape();
    if (pos.y < shape.min_y || pos.y >= shape.min_y + shape.height) {
        return false;
    }

    // Far enough from every player, and inside the world at all.
    for (const Vec3d& player : environment.players) {
        const f64 dx = player.x - (static_cast<f64>(pos.x) + 0.5);
        const f64 dy = player.y - static_cast<f64>(pos.y);
        const f64 dz = player.z - (static_cast<f64>(pos.z) + 0.5);
        if (dx * dx + dy * dy + dz * dz < kMinimumPlayerDistance * kMinimumPlayerDistance) {
            return false;
        }
    }

    const MobSize size = MobSize::from_box(width, height);

    if (rules.aquatic) {
        // Every block of the body in water, and a floor of some kind under it
        // so a fish is not spawned in a one-block puddle in the sky.
        for (i32 dx = 0; dx < size.width; ++dx) {
            for (i32 dz = 0; dz < size.width; ++dz) {
                for (i32 dy = 0; dy < size.height; ++dy) {
                    if (!is_water(level, {pos.x + dx, pos.y + dy, pos.z + dz})) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // A floor, and room for the body. Both are the pathfinder's questions, so
    // they are asked through the pathfinder's evaluator rather than answered
    // twice — a mob that may spawn somewhere it could not walk out of is a
    // mob that will be standing in a wall forever.
    const WalkNodeEvaluator walk;
    PathAbilities           abilities;
    abilities.enters_water = false;
    if (walk.type_at(level, pos, size, abilities) != PathNodeType::Walkable) {
        return false;
    }

    if (rules.max_spawn_light >= 0) {
        if (environment.light == nullptr) {
            return false;  // refused, not assumed dark
        }
        if (environment.light->effective_light(pos) > static_cast<u8>(rules.max_spawn_light)) {
            return false;
        }
    }

    if (category == MobCategory::Creature) {
        // An animal needs grass under it and sky above. Both are measured
        // properties of the world rather than of the animal, which is why they
        // are here and not in a per-type predicate.
        const registry::BlockRegistry& blocks = level.blocks();
        const registry::BlockStateId   floor  = level.block_at(pos.below());
        if (blocks.block_name(blocks.block_of(floor)) != "minecraft:grass_block") {
            return false;
        }
        if (environment.light == nullptr || environment.light->sky_light(pos) < 9) {
            return false;
        }
    }
    return true;
}

void NaturalSpawner::spawn_tick(const SpawnEnvironment& environment,
                                std::vector<SpawnRequest>& out) {
    if (environment.level == nullptr || environment.registries == nullptr) {
        return;
    }
    if (environment.players.empty() || environment.ticking_chunks.empty()) {
        // No players means no spawning, which is the game's rule and not an
        // optimisation: the vanilla spawner runs over chunks a player ticket
        // reaches, and with none it does nothing. Measured the hard way — a
        // whole run of scripts/measure_mobs.py read zero everywhere before the
        // rig learned to keep a probe client connected.
        return;
    }

    const auto entity_registry = environment.registries->find("minecraft:entity_type");
    if (!entity_registry) {
        return;
    }

    for (usize index = 0; index < 8; ++index) {
        const MobCategory category = static_cast<MobCategory>(index);
        if (category == MobCategory::Misc || entries(category).empty()) {
            continue;
        }
        const i32 cap = effective_cap(category, environment.ticking_chunks.size());
        if (cap <= 0) {
            continue;
        }
        const i32 live = index < environment.live_per_category.size()
                             ? environment.live_per_category[index]
                             : 0;
        if (live >= cap) {
            continue;
        }
        i32 budget = cap - live;

        for (const ChunkPos chunk : environment.ticking_chunks) {
            if (budget <= 0) {
                break;
            }
            for (i32 attempt = 0; attempt < kAttemptsPerChunk && budget > 0; ++attempt) {
                // Column first, height second — the game's order, and it
                // matters: drawing a y uniformly over the whole build height
                // would put nearly every attempt in the stone and make the
                // surface almost empty.
                const i32 x = chunk.min_block_x() + random_.next_int(kSectionSize);
                const i32 z = chunk.min_block_z() + random_.next_int(kSectionSize);
                const world::WorldShape shape = environment.level->shape();
                const i32               y     = shape.min_y + random_.next_int(shape.height);
                const BlockPos          pos{x, y, z};

                // Which type. A weighted draw over the category's entries, so
                // a biome that lists four zombies and one witch gets four
                // zombies and one witch.
                const std::vector<SpawnerEntry>& list = entries(category);
                i32                              total = 0;
                for (const SpawnerEntry& entry : list) {
                    total += entry.weight;
                }
                if (total <= 0) {
                    break;
                }
                i32                 roll   = random_.next_int(total);
                const SpawnerEntry* chosen = &list.front();
                for (const SpawnerEntry& entry : list) {
                    roll -= entry.weight;
                    if (roll < 0) {
                        chosen = &entry;
                        break;
                    }
                }

                const auto type = environment.registries->protocol_id(*entity_registry,
                                                                      chosen->type_name);
                if (!type) {
                    continue;  // named and refused, not silently skipped
                }
                const auto info = environment.registries->entity_type(*type);
                if (!info) {
                    continue;
                }
                if (!can_spawn_at(environment, category, pos, info->width, info->height)) {
                    continue;
                }

                const i32 group =
                    chosen->min_group +
                    random_.next_int(std::max(1, chosen->max_group - chosen->min_group + 1));
                const i32 pack = next_pack_++;
                for (i32 member = 0; member < group && budget > 0; ++member) {
                    // Pack members are offered positions around the leader's,
                    // and each is checked on its own: a herd on the edge of a
                    // cliff should be a smaller herd, not a herd in the air.
                    const BlockPos where =
                        member == 0 ? pos
                                    : BlockPos{pos.x + random_.next_int(11) - 5, pos.y,
                                               pos.z + random_.next_int(11) - 5};
                    if (member != 0 &&
                        !can_spawn_at(environment, category, where, info->width, info->height)) {
                        continue;
                    }
                    out.push_back(SpawnRequest{
                        chosen->type_name,
                        Vec3d{static_cast<f64>(where.x) + 0.5, static_cast<f64>(where.y),
                              static_cast<f64>(where.z) + 0.5},
                        category, pack});
                    --budget;
                }
            }
        }
    }
}

DespawnDecision decide_despawn(MobCategory category, f64 distance_to_nearest_player,
                               bool persistence_required, i32 idle_ticks,
                               math::LegacyRandomSource& random) {
    if (persistence_required) {
        return DespawnDecision::Keep;
    }
    const CategoryRules rules = rules_for(category);
    if (rules.despawn_distance == 0) {
        return DespawnDecision::Keep;  // Misc: an item is not a mob
    }
    if (distance_to_nearest_player > static_cast<f64>(rules.despawn_distance)) {
        return DespawnDecision::Immediate;
    }
    if (distance_to_nearest_player <= static_cast<f64>(rules.no_despawn_distance)) {
        return DespawnDecision::Keep;
    }
    if (idle_ticks < kIdleTicksBeforeDespawn) {
        return DespawnDecision::Keep;
    }
    // The draw happens whether or not it fires, so the stream advances the same
    // way for every mob in the intermediate band. Making the draw conditional
    // on something else would make the world depend on tick order.
    return random.next_int(kRandomDespawnOdds) == 0 ? DespawnDecision::Random
                                                    : DespawnDecision::Keep;
}

}  // namespace ov::gameplay
