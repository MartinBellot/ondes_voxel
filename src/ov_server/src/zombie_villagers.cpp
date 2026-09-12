// ── mobs-3 ── See zombie_villagers.hpp.
#define OV_LOG_CATEGORY "server"

#include "zombie_villagers.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/mob_logic.hpp"

#include <algorithm>
#include <cmath>

namespace ov::server {
namespace {

/// Metadata indices, read off the real server (villageois.md § 2).
constexpr u8 kConvertingIndex   = 19;
constexpr u8 kVillagerDataIndex = 20;

/// The measured cure: 3600 + a draw in 0..2400 (3734 to 5974 over 23 cures).
constexpr i32 kCureBase   = 3600;
constexpr i32 kCureSpread = 2401;

/// A villager's memory without what only a living villager has: its job site
/// and bed, a trade in progress, sleep, fright.
[[nodiscard]] gameplay::VillagerState remembered(const gameplay::VillagerState& villager) {
    gameplay::VillagerState out;
    out.type           = villager.type;
    out.profession     = villager.profession;
    out.level          = villager.level;
    out.xp             = villager.xp;
    out.offers         = villager.offers;
    out.offers_drawn   = villager.offers_drawn;
    out.restocks_today = villager.restocks_today;
    out.last_restock   = villager.last_restock;
    return out;
}

}  // namespace

ZombieVillagers::ZombieVillagers(const registry::Registries& registries) {
    if (const auto types = registries.find("minecraft:entity_type")) {
        zombie_villager_type_ =
            registries.protocol_id(*types, "minecraft:zombie_villager").value_or(-1);
        villager_type_ = registries.protocol_id(*types, "minecraft:villager").value_or(-1);
    }
    clicks_.reserve(8);
    handling_.reserve(8);
    gone_.reserve(8);
}

gameplay::VillagerState* ZombieVillagers::kept(i32 network_id) {
    return &records_[network_id].villager;
}

i32 ZombieVillagers::conversion_time(i32 network_id) const noexcept {
    const auto it = records_.find(network_id);
    return it == records_.end() ? -1 : it->second.conversion;
}

void ZombieVillagers::set_conversion_time(i32 network_id, i32 ticks) {
    records_[network_id].conversion = ticks;
}

bool ZombieVillagers::on_villager_killed(entity::EntityWorld& world, const MobKill& kill,
                                         gameplay::Difficulty difficulty,
                                         const ZombieVillagerHost& host) {
    if (kill.victim_type != "minecraft:villager" || !host.create_mob) {
        return false;
    }
    const bool zombie_family =
        kill.attacker_type == "minecraft:zombie" || kill.attacker_type == "minecraft:husk" ||
        kill.attacker_type == "minecraft:drowned" ||
        kill.attacker_type == "minecraft:zombie_villager";
    if (!zombie_family) {
        return false;
    }
    // Measured: Easy 0/10, Normal 37/90, Hard 10/10.
    if (difficulty == gameplay::Difficulty::Easy || difficulty == gameplay::Difficulty::Peaceful) {
        return false;
    }
    if (difficulty == gameplay::Difficulty::Normal && random_.next_int(2) != 0) {
        return false;
    }
    gameplay::VillagerState memory;
    if (const auto* villager = dynamic_cast<const gameplay::Mob*>(world.logic(kill.victim_handle))) {
        memory = remembered(villager->brain().villager);
    }
    const entity::EntityHandle risen = host.create_mob("minecraft:zombie_villager", kill.at);
    entity::EntityState*       state  = world.mutable_state(risen);
    if (state == nullptr) {
        return false;
    }
    state->yaw      = kill.yaw;
    state->head_yaw = kill.yaw;
    Record& record  = records_[state->network_id];
    record.villager = std::move(memory);
    if (host.announce) {
        host.announce(*state);
    }
    return true;
}

void ZombieVillagers::queue_interact(i32 player, i32 entity) {
    const std::scoped_lock lock{mutex_};
    clicks_.emplace_back(player, entity);
}

void ZombieVillagers::on_splash(const entity::EntityWorld& world, Vec3d at, i32 weakness_ticks) {
    if (weakness_ticks <= 0 || zombie_villager_type_ < 0) {
        return;
    }
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr || state->removed || state->type != zombie_villager_type_) {
            continue;
        }
        // The splash's box: four around the impact horizontally, two up and
        // down; the effect falls off with the distance to the mob's middle.
        const f64 dx = state->position.x - at.x;
        const f64 dy = state->position.y + static_cast<f64>(state->height) * 0.5 - at.y;
        const f64 dz = state->position.z - at.z;
        if (std::abs(dx) > 4.0 || std::abs(dz) > 4.0 || std::abs(dy) > 2.0 + static_cast<f64>(state->height)) {
            continue;
        }
        const f64 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= 4.0) {
            continue;
        }
        const f64 intensity = 1.0 - distance / 4.0;
        const i32 ticks     = static_cast<i32>(intensity * static_cast<f64>(weakness_ticks) + 0.5);
        if (ticks > 20) {
            Record& record  = records_[state->network_id];
            record.weakness = std::max(record.weakness, ticks);
        }
    }
}

i32 ZombieVillagers::cure_speed(const entity::EntityState& zombie, const ZombieVillagerHost& host) {
    // The wiki's rule: one tick in a hundred, every iron bar and bed within a
    // cube of four around it has three chances in ten to take a tick more off,
    // counting fourteen blocks at most.
    i32 speed = 1;
    if (random_.next_float() >= 0.01F || !host.speeds_cure) {
        return speed;
    }
    const BlockPos centre{static_cast<i32>(std::floor(zombie.position.x)),
                          static_cast<i32>(std::floor(zombie.position.y)),
                          static_cast<i32>(std::floor(zombie.position.z))};
    i32 counted = 0;
    for (i32 x = centre.x - 4; x < centre.x + 4 && counted < 14; ++x) {
        for (i32 y = centre.y - 4; y < centre.y + 4 && counted < 14; ++y) {
            for (i32 z = centre.z - 4; z < centre.z + 4 && counted < 14; ++z) {
                if (host.speeds_cure(BlockPos{x, y, z})) {
                    if (random_.next_float() < 0.3F) {
                        ++speed;
                    }
                    ++counted;
                }
            }
        }
    }
    return speed;
}

void ZombieVillagers::finish_cure(entity::EntityWorld& world, entity::EntityState& zombie,
                                  Record& record, const ZombieVillagerHost& host) {
    zombie.removed = true;
    if (!host.create_mob) {
        return;
    }
    const entity::EntityHandle cured = host.create_mob("minecraft:villager", zombie.position);
    entity::EntityState*       state = world.mutable_state(cured);
    if (state == nullptr) {
        return;
    }
    state->yaw      = zombie.yaw;
    state->head_yaw = zombie.yaw;
    if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(cured))) {
        gameplay::VillagerState& villager = mob->mutable_brain().villager;
        villager.type           = record.villager.type;
        villager.profession     = record.villager.profession;
        villager.level          = record.villager.level;
        villager.xp             = record.villager.xp;
        villager.offers         = record.villager.offers;
        villager.offers_drawn   = record.villager.offers_drawn;
        villager.restocks_today = record.villager.restocks_today;
        villager.last_restock   = record.villager.last_restock;
        // ── brains ── its gossip and its type come back with it
        villager.gossips           = record.villager.gossips;
        villager.last_gossip_decay = record.villager.last_gossip_decay;
        villager.typed             = true;
        ++villager.revision;
    }
    if (host.announce) {
        host.announce(*state);
    }
    if (host.on_cured && record.curer >= 0) {  // ── brains ──
        host.on_cured(state->network_id, record.curer);
    }
}

ZombieVillagerStats ZombieVillagers::tick(entity::EntityWorld& world,
                                          const ZombieVillagerHost& host,
                                          const AttackDeliver& deliver) {
    ZombieVillagerStats stats;
    {
        const std::scoped_lock lock{mutex_};
        handling_.swap(clicks_);
    }
    for (const auto& [player, entity] : handling_) {
        const entity::EntityHandle handle = world.find(entity);
        entity::EntityState*       state =
            handle == entity::kNoEntity ? nullptr : world.mutable_state(handle);
        if (state == nullptr || state->removed || state->type != zombie_villager_type_) {
            continue;
        }
        Record& record = records_[state->network_id];
        // The cure asks for a golden apple, on a zombie villager under
        // Weakness, that is not already curing.
        if (record.conversion >= 0 || record.weakness <= 0 || !host.held ||
            host.held(player) != "minecraft:golden_apple") {
            continue;
        }
        if (host.consume_held && !(host.creative && host.creative(player))) {
            host.consume_held(player);
        }
        record.conversion = kCureBase + random_.next_int(kCureSpread);
        record.weakness   = 0;
        record.curer      = player;  // ── brains ──
        ++stats.cures_started;
        net::MetadataWriter fields;
        fields.boolean_value(kConvertingIndex, true);
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state->network_id, fields.take()));
        // Entity Event 16: the zombie villager's cure sound and shake.
        deliver(net::clientbound::kEntityEvent, net::encode_entity_event(state->network_id, 16));
        OV_LOG_DEBUG("zombie villager {}: cure started, {} ticks", state->network_id,
                     record.conversion);
    }
    handling_.clear();

    gone_.clear();
    for (auto& [id, record] : records_) {
        const entity::EntityHandle handle = world.find(id);
        entity::EntityState*       state =
            handle == entity::kNoEntity ? nullptr : world.mutable_state(handle);
        if (state == nullptr) {
            gone_.push_back(id);
            continue;
        }
        if (state->removed) {
            continue;
        }
        if (record.weakness > 0) {
            --record.weakness;
        }
        if (record.conversion < 0) {
            continue;
        }
        record.conversion -= cure_speed(*state, host);
        if (record.conversion <= 0) {
            finish_cure(world, *state, record, host);
            gone_.push_back(id);
            ++stats.cured;
        }
    }
    for (const i32 id : gone_) {
        records_.erase(id);
    }
    return stats;
}

void ZombieVillagers::spawn_metadata(const entity::EntityState& state,
                                     net::MetadataWriter&       fields) const {
    if (state.type != zombie_villager_type_) {
        return;
    }
    const auto it = records_.find(state.network_id);
    if (it == records_.end()) {
        fields.villager_data_value(kVillagerDataIndex, 2, 0, 1);  // plains, none, novice
        return;
    }
    const gameplay::VillagerState& v = it->second.villager;
    fields.villager_data_value(kVillagerDataIndex, static_cast<i32>(v.type),
                               static_cast<i32>(v.profession), v.level);
    if (it->second.conversion >= 0) {
        fields.boolean_value(kConvertingIndex, true);
    }
}

}  // namespace ov::server
