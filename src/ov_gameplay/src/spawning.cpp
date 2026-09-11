#include "ov/gameplay/spawning.hpp"

#include "ov/gameplay/pathfinding.hpp"
#include "ov/gameplay/spawn_rules.hpp"  // ── mobs-2 ──

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

    // ── mobs-2 ── the rest of what an overworld biome file lists, so that a
    // mob the biome spawns is counted against its own category's cap.
    {"minecraft:cat", MobCategory::Creature},
    {"minecraft:frog", MobCategory::Creature},
    {"minecraft:polar_bear", MobCategory::Creature},
    {"minecraft:mooshroom", MobCategory::Creature},
    {"minecraft:panda", MobCategory::Creature},
    {"minecraft:parrot", MobCategory::Creature},
    {"minecraft:turtle", MobCategory::Creature},
    {"minecraft:ocelot", MobCategory::Creature},
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

bool NaturalSpawner::position_plausible(const SpawnEnvironment& environment,
                                        MobCategory category, BlockPos pos) const {
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
    if (!level.shape().contains_y(pos.y)) {
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

    // The cell the mob's feet would be in, before anything is known about the
    // mob. A block that stops movement cannot hold one whatever its size, and
    // this is the check that does the work: a y drawn uniformly over the build
    // height lands in stone the overwhelming majority of the time.
    //
    // It is also the one vanilla asks first, and leaving it out is why the
    // first version of this reordering saved nothing measurable — 99.8% of
    // attempts still reached the type draw. With it, see the number in
    // docs/provenance/redstone.md.
    if (!rules.aquatic) {
        const registry::BlockRegistry& blocks = level.blocks();
        if (blocks.blocks_motion(blocks.block_of(level.block_at(pos)))) {
            return false;
        }
    }

    // ── mobs-2 ── A monster's light is a draw, not a threshold (spawn_rules.hpp
    // and docs/provenance/mobs-2.md § 3). What is asked here is only the part
    // no draw can rescue — block light over the dimension's limit of 0, or a
    // light level above the provider's 7 — so a lit position costs no draw.
    // The draw itself is the type's (`can_spawn_type_at`): a slime has its own.
    if (category == MobCategory::Monster) {
        if (environment.light == nullptr) {
            return false;  // refused, not assumed dark
        }
        if (!monster_light_possible(*environment.light, pos)) {
            return false;
        }
    } else if (rules.max_spawn_light >= 0) {
        if (environment.light == nullptr) {
            return false;  // refused, not assumed dark
        }
        if (environment.light->effective_light(pos) > static_cast<u8>(rules.max_spawn_light)) {
            return false;
        }
    }

    if (category == MobCategory::Creature) {
        // An animal needs a raw light level above 8 — sky or block, undimmed
        // by the hour. Which floor it needs is the type's (a rabbit stands on
        // sand, a wolf on snow), so the floor is asked after the draw.
        if (environment.light == nullptr ||
            std::max(environment.light->sky_light(pos), environment.light->block_light(pos)) < 9) {
            return false;
        }
    }
    return true;
}

bool NaturalSpawner::can_spawn_at(const SpawnEnvironment& environment, MobCategory category,
                                  BlockPos pos, f32 width, f32 height) const {
    if (!position_plausible(environment, category, pos)) {
        return false;
    }
    const world::LevelView& level = *environment.level;
    const CategoryRules     rules = rules_for(category);
    const MobSize           size  = MobSize::from_box(width, height);

    // ── mobs-2 ── With no type named, an animal stands on the game's default
    // floor, `#animals_spawnable_on` (grass). A type with its own floor goes
    // through `can_spawn_type_at` instead.
    if (category == MobCategory::Creature &&
        !floor_in_tag(environment, pos.below(), "minecraft:animals_spawnable_on")) {
        return false;
    }

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
    return walk.type_at(level, pos, size, abilities) == PathNodeType::Walkable;
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

    // The passive pass runs once every four hundred ticks, not every tick.
    //
    // `game_time` -1 means the caller keeps no clock; counting our own calls is
    // then exactly as good, because `spawn_tick` is defined as one tick's
    // worth. It is a named fallback and not a default value: reading -1 as tick
    // zero would make `% 400` true on every call, which is the bug this gate
    // exists to remove.
    const i64 clock = environment.game_time >= 0 ? environment.game_time : ticks_;
    ++ticks_;
    const bool passive_pass = clock % kPassiveSpawnInterval == 0;

    for (usize index = 0; index < 8; ++index) {
        const MobCategory category = static_cast<MobCategory>(index);
        if (category == MobCategory::Misc || !category_has_entries(category)) {  // ── mobs-2 ──
            continue;
        }
        if (category == MobCategory::Creature && !passive_pass) {
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
                const i32               y =
                    shape.min_y + random_.next_int(static_cast<i32>(shape.height));
                const BlockPos          pos{x, y, z};

                // The position first, and only then the type.
                //
                // Everything a position can be rejected for that does not need
                // a hitbox — unloaded, out of the world, under a player's feet,
                // too bright, not on grass — is asked here, before a single
                // weighted draw and before a single name is resolved. Nearly
                // every attempt dies at this line: a uniform y over the build
                // height puts most of them in stone.
                //
                // Drawing the type first was measurable: it made
                // `Registries::protocol_id` a hot function, resolving a mob
                // name by string once per attempt, and that is the reason a
                // hash index was added to `ov_registry` to compensate. With the
                // check in this order the index has no hot caller left.
                if (!position_plausible(environment, category, pos)) {
                    continue;
                }

                // Which type. A weighted draw over the category's entries, so
                // a biome that lists four zombies and one witch gets four
                // zombies and one witch.
                // ── mobs-2 ── From the biome **of this position**.
                const std::vector<SpawnerEntry>& list = entries_at(environment, category, pos);
                i32                              total = 0;
                for (const SpawnerEntry& entry : list) {
                    total += entry.weight;
                }
                if (total <= 0) {
                    continue;  // this biome spawns nothing of the category
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
                if (!can_spawn_type_at(environment, category, chosen->type_name, pos,
                                       info->width, info->height)) {  // ── mobs-2 ──
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
                        !can_spawn_type_at(environment, category, chosen->type_name, where,
                                           info->width, info->height)) {  // ── mobs-2 ──
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

// ── mobs-2 ── Biome lists and the type's own predicate ───────────────────────

void NaturalSpawner::set_biome_entries(u16 biome, MobCategory category,
                                       std::span<const SpawnerEntry> entries) {
    if (biome >= biomes_.size()) {
        biomes_.resize(static_cast<usize>(biome) + 1);
    }
    std::vector<SpawnerEntry>& list = biomes_[biome].lists[category_index(category)];
    list.assign(entries.begin(), entries.end());
}

void NaturalSpawner::set_surface_slimes(u16 biome, bool allowed) {
    if (biome >= biomes_.size()) {
        biomes_.resize(static_cast<usize>(biome) + 1);
    }
    biomes_[biome].surface_slimes = allowed;
}

bool NaturalSpawner::category_has_entries(MobCategory category) const noexcept {
    if (!entries(category).empty()) {
        return true;
    }
    for (const BiomeLists& biome : biomes_) {
        if (!biome.lists[category_index(category)].empty()) {
            return true;
        }
    }
    return false;
}

const std::vector<SpawnerEntry>& NaturalSpawner::entries_at(const SpawnEnvironment& environment,
                                                            MobCategory category,
                                                            BlockPos    pos) const noexcept {
    if (environment.biomes == nullptr || biomes_.empty()) {
        return entries(category);
    }
    const u16 biome = environment.biomes->biome_at(pos);
    if (biome >= biomes_.size()) {
        // A biome no file was loaded for spawns nothing — refused, not given
        // the category-wide list, which would be a plains list in a desert.
        static const std::vector<SpawnerEntry> kNone;
        return kNone;
    }
    return biomes_[biome].lists[category_index(category)];
}

bool NaturalSpawner::floor_in_tag(const SpawnEnvironment& environment, BlockPos floor,
                                  std::string_view tag) const {
    const registry::Registries* registries = environment.registries;
    if (registries == nullptr || environment.level == nullptr) {
        return false;
    }
    const auto block_registry = registries->find("minecraft:block");
    if (!block_registry) {
        return false;
    }
    const auto found = registries->find_tag(*block_registry, tag);
    if (!found) {
        return false;  // a tag the pack does not carry is refused, not "anything"
    }
    const registry::BlockRegistry& blocks = environment.level->blocks();
    const auto id = registries->protocol_id(
        *block_registry, blocks.block_name(blocks.block_of(environment.level->block_at(floor))));
    return id && registries->tag_contains(*found, *id);
}

bool NaturalSpawner::can_spawn_type_at(const SpawnEnvironment& environment, MobCategory category,
                                       std::string_view type_name, BlockPos pos, f32 width,
                                       f32 height) {
    const TypeSpawnRule rule = spawn_rule_of(type_name, category == MobCategory::Creature);

    if (rule.rule == SpawnRule::Animal) {
        // The category's checks, then the type's floor instead of grass.
        if (!position_plausible(environment, category, pos)) {
            return false;
        }
        if (!floor_in_tag(environment, pos.below(), rule.floor_tag)) {
            return false;
        }
        const WalkNodeEvaluator walk;
        PathAbilities           abilities;
        abilities.enters_water = false;
        return walk.type_at(*environment.level, pos, MobSize::from_box(width, height), abilities) ==
               PathNodeType::Walkable;
    }

    if (category != MobCategory::Monster || environment.light == nullptr) {
        return can_spawn_at(environment, category, pos, width, height);
    }

    if (rule.rule == SpawnRule::Slime) {
        // minecraft.wiki *Slime*: a surface-slime biome between y 51 and 69,
        // half the time times the moon, and a light draw; or a slime chunk
        // below y 40, one attempt in ten. No monster darkness draw.
        bool allowed = false;
        const u16 biome = environment.biomes != nullptr ? environment.biomes->biome_at(pos) : 0;
        if (environment.biomes != nullptr && biome < biomes_.size() &&
            biomes_[biome].surface_slimes && pos.y > 50 && pos.y < 70 &&
            environment.day_time >= 0) {
            const f32 half  = random_.next_float();
            const f32 moon  = random_.next_float();
            const i32 light = random_.next_int(8);
            allowed         = half < 0.5F && moon < moon_brightness(environment.day_time) &&
                      static_cast<i32>(environment.light->effective_light(pos)) <= light;
        }
        if (!allowed) {
            const i32 tenth = random_.next_int(10);
            allowed = tenth == 0 && pos.y < 40 &&
                      is_slime_chunk(environment.world_seed, pos.x >> 4, pos.z >> 4);
        }
        return allowed && can_spawn_at(environment, category, pos, width, height);
    }

    if (!monster_dark_enough(*environment.light, pos, random_)) {
        return false;
    }
    if (rule.rule == SpawnRule::MonsterUnderSky && environment.light->sky_light(pos) < 15) {
        // "Can see the sky", approximated by unobstructed sky light: a glass
        // roof lets 15 through and the game's heightmap test would not. Named
        // in docs/provenance/mobs-2.md.
        return false;
    }
    return can_spawn_at(environment, category, pos, width, height);
}
// ── end mobs-2 ──

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
