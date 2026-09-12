#include "villager_life.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/wandering_trader.hpp"

#include <array>
#include <cmath>

namespace ov::server {
namespace {

using gameplay::VillagerType;

struct BiomeType {
    std::string_view biome;
    VillagerType     type;
};

// Measured: a villager summoned bare in each of 53 biomes (fillbiome), its
// VillagerData.type read back (measure_villager_life.py `biome`). Every biome
// not listed gave plains.
constexpr std::array<BiomeType, 29> kBiomeTypes{{
    {"minecraft:desert", VillagerType::Desert},
    {"minecraft:badlands", VillagerType::Desert},
    {"minecraft:eroded_badlands", VillagerType::Desert},
    {"minecraft:wooded_badlands", VillagerType::Desert},
    {"minecraft:jungle", VillagerType::Jungle},
    {"minecraft:sparse_jungle", VillagerType::Jungle},
    {"minecraft:bamboo_jungle", VillagerType::Jungle},
    {"minecraft:savanna", VillagerType::Savanna},
    {"minecraft:savanna_plateau", VillagerType::Savanna},
    {"minecraft:windswept_savanna", VillagerType::Savanna},
    {"minecraft:snowy_plains", VillagerType::Snow},
    {"minecraft:ice_spikes", VillagerType::Snow},
    {"minecraft:snowy_taiga", VillagerType::Snow},
    {"minecraft:grove", VillagerType::Snow},
    {"minecraft:snowy_slopes", VillagerType::Snow},
    {"minecraft:frozen_peaks", VillagerType::Snow},
    {"minecraft:jagged_peaks", VillagerType::Snow},
    {"minecraft:frozen_river", VillagerType::Snow},
    {"minecraft:snowy_beach", VillagerType::Snow},
    {"minecraft:frozen_ocean", VillagerType::Snow},
    {"minecraft:deep_frozen_ocean", VillagerType::Snow},
    {"minecraft:swamp", VillagerType::Swamp},
    {"minecraft:mangrove_swamp", VillagerType::Swamp},
    {"minecraft:taiga", VillagerType::Taiga},
    {"minecraft:old_growth_pine_taiga", VillagerType::Taiga},
    {"minecraft:old_growth_spruce_taiga", VillagerType::Taiga},
    {"minecraft:windswept_hills", VillagerType::Taiga},
    {"minecraft:windswept_gravelly_hills", VillagerType::Taiga},
    {"minecraft:windswept_forest", VillagerType::Taiga},
}};

struct SeedCrop {
    std::string_view seed;
    std::string_view crop;
};
constexpr std::array<SeedCrop, 6> kSeedCrops{{
    {"minecraft:wheat_seeds", "minecraft:wheat"},
    {"minecraft:beetroot_seeds", "minecraft:beetroots"},
    {"minecraft:carrot", "minecraft:carrots"},
    {"minecraft:potato", "minecraft:potatoes"},
    {"minecraft:torchflower_seeds", "minecraft:torchflower_crop"},
    {"minecraft:pitcher_pod", "minecraft:pitcher_crop"},
}};

/// A villager seen by a killing: this close to the victim. The wiki's
/// witnesses are the ones the victim sees; the sight is not modelled here.
constexpr f64 kWitnessReach = 16.0;

[[nodiscard]] gameplay::VillagerState* villager_at(entity::EntityWorld& world,
                                                   entity::EntityHandle handle) {
    gameplay::MobBrain* b = gameplay::mob_brain_of(world, handle);
    return b != nullptr && b->villager.active && !b->villager.wandering ? &b->villager : nullptr;
}

[[nodiscard]] BlockPos block_of(const Vec3d& p) noexcept {
    return BlockPos{static_cast<i32>(std::floor(p.x)), static_cast<i32>(std::floor(p.y)),
                    static_cast<i32>(std::floor(p.z))};
}

}  // namespace

VillagerType villager_type_for_biome(std::string_view biome) noexcept {
    constexpr std::string_view kNamespace = "minecraft:";
    if (biome.starts_with(kNamespace)) {
        biome.remove_prefix(kNamespace.size());
    }
    for (const BiomeType& b : kBiomeTypes) {
        if (b.biome.substr(kNamespace.size()) == biome) {
            return b.type;
        }
    }
    return VillagerType::Plains;
}

std::string_view crop_of_seed(std::string_view seed) noexcept {
    for (const SeedCrop& s : kSeedCrops) {
        if (s.seed == seed) {
            return s.crop;
        }
    }
    return {};
}

VillagerLife::VillagerLife(const registry::Registries& registries, u64 seed)
    : random_{static_cast<i64>(seed ^ 0x4C49'4645ULL)} {
    events_.reserve(16);
    drops_.reserve(8);
    if (const auto types = registries.find("minecraft:entity_type")) {
        villager_type_ =
            static_cast<i32>(registries.protocol_id(*types, "minecraft:villager").value_or(-1));
        iron_golem_type_ =
            static_cast<i32>(registries.protocol_id(*types, "minecraft:iron_golem").value_or(-1));
    }
}

void VillagerLife::attach(gameplay::VillagerWorld& world) {
    world.events          = &events_;
    world.iron_golem_type = iron_golem_type_;
    world.villager_type   = villager_type_;
}

VillagerLifeStats VillagerLife::before_entity_tick(entity::EntityWorld& world, i64 game_time,
                                                   const VillagerLifeHost& host) {
    VillagerLifeStats stats;
    events_.clear();
    for (const entity::EntityHandle handle : world.handles()) {
        // A wandering trader counts its DespawnDelay down, and leaves at 0.
        if (gameplay::MobBrain* b = gameplay::mob_brain_of(world, handle);
            b != nullptr && b->villager.active && b->villager.wandering) {
            if (gameplay::tick_despawn(b->villager)) {
                if (entity::EntityState* gone = world.mutable_state(handle)) {
                    gone->removed = true;
                    ++stats.traders_left;
                }
            }
            continue;
        }
        gameplay::VillagerState*   v     = villager_at(world, handle);
        const entity::EntityState* state = world.state(handle);
        if (v == nullptr || state == nullptr || state->removed) {
            continue;
        }
        if (!v->typed) {
            v->typed = true;
            if (host.biome_at) {
                const std::string_view biome = host.biome_at(block_of(state->position));
                if (!biome.empty()) {
                    const VillagerType type = villager_type_for_biome(biome);
                    if (type != v->type) {
                        v->type = type;
                        ++v->revision;
                    }
                }
            }
            ++stats.typed;
        }
        if (gameplay::brain::maybe_decay(v->gossips, v->last_gossip_decay, game_time)) {
            ++stats.decays;
        }
    }
    return stats;
}

VillagerLifeStats VillagerLife::after_entity_tick(entity::EntityWorld& world,
                                                  const VillagerLifeHost& host) {
    using Kind = gameplay::brain::VillagerEventKind;
    VillagerLifeStats stats;
    for (const gameplay::brain::VillagerEvent& e : events_) {
        const entity::EntityState* self = world.state(e.self);
        switch (e.kind) {
            case Kind::Birth: {
                const entity::EntityState* other = world.state(e.other);
                gameplay::VillagerState*   a     = villager_at(world, e.self);
                gameplay::VillagerState*   b     = villager_at(world, e.other);
                if (!host.create_mob || a == nullptr || b == nullptr || other == nullptr) {
                    break;
                }
                const entity::EntityHandle child = host.create_mob("minecraft:villager", e.at);
                entity::EntityState*       cs    = world.mutable_state(child);
                auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(child));
                if (cs == nullptr || mob == nullptr) {
                    break;
                }
                mob->make_baby(*cs);
                gameplay::VillagerState& cv = mob->mutable_brain().villager;
                cv.claims.home              = e.block;
                cv.typed                    = true;
                // Half the time the biome's type, else one parent's or the
                // other's (the wiki); the breed_type campaign measures it.
                const f32 d = random_.next_float();
                if (d < 0.5F) {
                    cv.type = host.biome_at ? villager_type_for_biome(host.biome_at(block_of(e.at)))
                                            : a->type;
                } else {
                    cv.type = d < 0.75F ? a->type : b->type;
                }
                ++cv.revision;
                if (host.announce) {
                    host.announce(*cs);
                }
                ++stats.births;
                break;
            }
            case Kind::NoBed:
                if (host.entity_event) {
                    if (self != nullptr) {
                        host.entity_event(self->network_id, 13);
                    }
                    if (const entity::EntityState* other = world.state(e.other)) {
                        host.entity_event(other->network_id, 13);
                    }
                }
                ++stats.no_bed;
                break;
            case Kind::SummonGolem: {
                if (!host.create_mob) {
                    break;
                }
                const entity::EntityHandle golem = host.create_mob("minecraft:iron_golem", e.at);
                if (const entity::EntityState* gs = world.state(golem)) {
                    if (host.announce) {
                        host.announce(*gs);
                    }
                    ++stats.golems;
                    OV_LOG_DEBUG("villagers: an iron golem summoned at {} {} {}", e.block.x,
                                 e.block.y, e.block.z);
                }
                break;
            }
            case Kind::Harvest: {
                gameplay::VillagerState* v = villager_at(world, e.self);
                if (v == nullptr || !host.harvest) {
                    break;
                }
                drops_.clear();
                host.harvest(e.block, drops_);
                // What the farmer picks up goes in its pockets; what it would
                // not pick up (or has no room for) is not kept — named.
                for (const auto& [item, count] : drops_) {
                    (void)gameplay::brain::pocket(*v, item, count);
                }
                ++stats.harvests;
                break;
            }
            case Kind::Plant: {
                const std::string_view crop = crop_of_seed(e.item);
                if (!crop.empty() && host.place) {
                    host.place(e.block, crop);
                    ++stats.plants;
                }
                break;
            }
        }
    }
    events_.clear();
    return stats;
}

bool VillagerLife::tick_trader_spawner(entity::EntityWorld& world, std::span<const Vec3d> players,
                                       bool allowed, const VillagerLifeHost& host) {
    if (!trader_spawner_.tick(random_, allowed) || players.empty() || !host.create_mob ||
        !host.surface) {
        return false;
    }
    // The wiki: then one chance in ten, for one player picked at random.
    if (random_.next_int(10) != 0) {
        return false;
    }
    const Vec3d& who = players[static_cast<usize>(random_.next_int(static_cast<i32>(players.size())))];
    for (i32 attempt = 0; attempt < 10; ++attempt) {
        const i32 x = static_cast<i32>(std::floor(who.x)) + random_.next_int(97) - 48;
        const i32 z = static_cast<i32>(std::floor(who.z)) + random_.next_int(97) - 48;
        const auto y = host.surface(x, z);
        if (!y) {
            continue;
        }
        const Vec3d at{static_cast<f64>(x) + 0.5, static_cast<f64>(*y), static_cast<f64>(z) + 0.5};
        const entity::EntityHandle trader = host.create_mob("minecraft:wandering_trader", at);
        const entity::EntityState* ts     = world.state(trader);
        if (ts == nullptr) {
            return false;
        }
        if (gameplay::MobBrain* b = gameplay::mob_brain_of(world, trader)) {
            b->villager.despawn_delay = gameplay::kTraderDespawnDelay;
        }
        if (host.announce) {
            host.announce(*ts);
        }
        for (const f64 side : {2.0, -2.0}) {
            const entity::EntityHandle llama =
                host.create_mob("minecraft:trader_llama", Vec3d{at.x + side, at.y, at.z});
            if (const entity::EntityState* ls = world.state(llama); ls != nullptr && host.announce) {
                host.announce(*ls);
            }
        }
        trader_spawner_.came();
        OV_LOG_INFO("a wandering trader came, at {} {} {}", x, *y, z);
        return true;
    }
    return false;
}

void VillagerLife::on_player_hurt(entity::EntityWorld& world, i32 villager,
                                  const net::Uuid& player) {
    if (gameplay::VillagerState* v = villager_at(world, world.find(villager))) {
        v->gossips.add_event(player, gameplay::brain::ReputationEvent::VillagerHurt);
    }
}

void VillagerLife::on_player_killed(entity::EntityWorld& world, Vec3d where, i32 victim,
                                    const net::Uuid& player) {
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* s = world.state(handle);
        gameplay::VillagerState*   v = villager_at(world, handle);
        if (s == nullptr || v == nullptr || s->network_id == victim || s->removed) {
            continue;
        }
        const f64 dx = s->position.x - where.x;
        const f64 dy = s->position.y - where.y;
        const f64 dz = s->position.z - where.z;
        if (dx * dx + dy * dy + dz * dz <= kWitnessReach * kWitnessReach) {
            v->gossips.add_event(player, gameplay::brain::ReputationEvent::VillagerKilled);
        }
    }
}

void VillagerLife::on_cured(entity::EntityWorld& world, i32 villager, const net::Uuid& player) {
    if (gameplay::VillagerState* v = villager_at(world, world.find(villager))) {
        v->gossips.add_event(player, gameplay::brain::ReputationEvent::ZombieVillagerCured);
    }
}

}  // namespace ov::server
