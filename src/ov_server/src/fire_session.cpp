#define OV_LOG_CATEGORY "fire"

#include "fire_session.hpp"

#include "survival_session.hpp"

#include "ov/base/log.hpp"
#include "ov/protocol/entity.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace ov::server {

namespace {

/// `#minecraft:increased_fire_burnout` in 1.20.1's datapack
/// (data/minecraft/tags/worldgen/biome/increased_fire_burnout.json).
constexpr std::array<std::string_view, 8> kHumidBiomes{
    "minecraft:bamboo_jungle", "minecraft:mushroom_fields", "minecraft:mangrove_swamp",
    "minecraft:snowy_slopes",  "minecraft:frozen_peaks",    "minecraft:jagged_peaks",
    "minecraft:swamp",         "minecraft:jungle"};

/// `Biome.warmEnoughToRain`: 0.15. Above y = 80 the game also lowers the
/// temperature by `(noise * 8 + y - 80) * 0.05 / 40`; the noise is not
/// carried here and is taken as zero — named in docs/provenance/feu.md.
constexpr f64 kRainTemperature = 0.15;

[[nodiscard]] std::vector<u8> flag_payload(i32 entity_id, bool on_fire, u8 other_bits) {
    net::MetadataWriter fields;
    fields.byte_value(net::metadata::kSharedFlags,
                      static_cast<i8>(static_cast<u8>(other_bits | (on_fire ? 0x01 : 0))));
    return net::encode_entity_metadata(entity_id, fields.take());
}

}  // namespace

FireSession::FireSession(const registry::BlockRegistry& blocks,
                         const registry::Registries& registries, FireHost host,
                         MobCombat* mob_combat)
    : blocks_{&blocks},
      rules_{blocks, registries},
      host_{std::move(host)},
      mob_combat_{mob_combat} {
    humid_.assign(blocks.biome_count(), 0);
    for (const std::string_view name : kHumidBiomes) {
        if (const auto index = blocks.find_biome(name); index && *index < humid_.size()) {
            humid_[*index] = 1;
        }
    }
    if (const auto types = registries.find("minecraft:entity_type")) {
        if (const auto id = registries.protocol_id(*types, "minecraft:zombie")) {
            zombie_type_ = *id;
        }
        if (const auto id = registries.protocol_id(*types, "minecraft:skeleton")) {
            skeleton_type_ = *id;
        }
    }
    if (rules_.unknown_names() > 0) {
        OV_LOG_WARN("{} rows of the flammability table name blocks this registry lacks",
                    rules_.unknown_names());
    }
    mobs_.reserve(64);
    gone_.reserve(64);
    pending_.reserve(16);
    applying_.reserve(16);
}

// ── Blocks ──────────────────────────────────────────────────────────────────

void FireSession::neighbour_changed(ServerLevel& level, BlockPos pos) {
    rules_.neighbour_changed(level, pos, random_);
}

bool FireSession::scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) {
    if (what != "minecraft:fire") {
        return false;
    }
    (void)rules_.scheduled_tick(level, *this, pos, random_);
    return true;
}

bool FireSession::ticks_randomly(registry::BlockStateId state) const noexcept {
    return rules_.ticks_randomly(state);
}

void FireSession::random_tick(world::LevelWriter& level, BlockPos pos,
                              registry::BlockStateId state) {
    (void)state;
    // Twice per pick, measured (fire.hpp, kLavaTicksPerPick).
    for (i32 i = 0; i < gameplay::FireRules::kLavaTicksPerPick; ++i) {
        (void)rules_.lava_random_tick(level, *this, pos, random_);
    }
}

bool FireSession::biome_rains(i32 biome, i32 y) const noexcept {
    if (biome < 0) {
        return false;
    }
    const registry::BiomeEffects effects = blocks_->biome(static_cast<u32>(biome));
    if (!effects.has_precipitation) {
        return false;
    }
    f64 temperature = effects.temperature;
    if (y > 80) {
        temperature -= static_cast<f64>(y - 80) * 0.05 / 40.0;
    }
    return temperature >= kRainTemperature;
}

bool FireSession::is_raining_at(BlockPos pos) const {
    if (!world_.raining || !host_.rain_top || !host_.biome_at) {
        return false;
    }
    if (host_.rain_top(pos.x, pos.z) > pos.y) {
        return false;
    }
    return biome_rains(host_.biome_at(pos), pos.y);
}

bool FireSession::increased_burnout(BlockPos pos) const {
    if (!host_.biome_at) {
        return false;
    }
    const i32 biome = host_.biome_at(pos);
    return biome >= 0 && static_cast<usize>(biome) < humid_.size() &&
           humid_[static_cast<usize>(biome)] != 0;
}

void FireSession::prime_tnt(BlockPos pos) {
    if (host_.prime_tnt) {
        host_.prime_tnt(pos);
    }
}

bool FireSession::ignite(ServerLevel& level, BlockPos pos) { return rules_.ignite(level, pos); }

// ── Contact ─────────────────────────────────────────────────────────────────

gameplay::FireContact FireSession::contact(Vec3d feet, f64 width, f64 height) const {
    gameplay::FireContact out;
    if (!host_.block_at) {
        return out;
    }
    // `checkInsideBlocks` walks the box shrunk by 1e-7; the fluid test walks
    // it shrunk by 0.001 and compares each fluid's surface to the box floor.
    const f64 half = width * 0.5;
    const f64 min_x = feet.x - half + 1.0e-7;
    const f64 max_x = feet.x + half - 1.0e-7;
    const f64 min_y = feet.y + 1.0e-7;
    const f64 max_y = feet.y + height - 1.0e-7;
    const f64 min_z = feet.z - half + 1.0e-7;
    const f64 max_z = feet.z + half - 1.0e-7;
    const f64 fluid_floor = feet.y + 0.001;

    const auto x0 = static_cast<i32>(std::floor(min_x));
    const auto x1 = static_cast<i32>(std::floor(max_x));
    const auto y0 = static_cast<i32>(std::floor(min_y));
    const auto y1 = static_cast<i32>(std::floor(max_y));
    const auto z0 = static_cast<i32>(std::floor(min_z));
    const auto z1 = static_cast<i32>(std::floor(max_z));
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                const BlockPos               at{x, y, z};
                const registry::BlockStateId state = host_.block_at(at);
                if (rules_.is_fire(state)) {
                    out.in_fire = true;
                    continue;
                }
                if (rules_.is_soul_fire(state)) {
                    out.in_soul_fire = true;
                    continue;
                }
                const registry::BlockId block = blocks_->block_of(state);
                if (rules_.is_campfire(state) || rules_.is_soul_campfire(state)) {
                    const auto lit = blocks_->find_property(block, "lit");
                    if (lit && blocks_->property_value(state, *lit) == "true") {
                        out.campfire = std::max<u8>(out.campfire,
                                                    rules_.is_soul_campfire(state) ? 2 : 1);
                    }
                    continue;
                }
                // ── implicit water ── the one registry query: the fluid block
                // at its level, a waterlogged block, or seagrass, kelp and a
                // bubble column, which are always a water source.
                const registry::BlockRegistry::StateFluid held = blocks_->fluid(state);
                if (held.empty()) {
                    continue;
                }
                const bool lava = held.is_lava();
                // A source and a falling column stand 8/9 of a block high, a
                // flowing level n stands (8 - n)/9.
                const i32 level = held.level;
                const i32 amount  = level == 0 || level >= 8 ? 8 : 8 - level;
                const f64 surface = static_cast<f64>(y) + static_cast<f64>(amount) / 9.0;
                if (surface >= fluid_floor) {
                    if (lava) {
                        out.in_lava = true;
                    } else {
                        out.in_water = true;
                        out.wet      = true;
                    }
                }
            }
        }
    }
    // Rain at the feet, or at the top of the box.
    if (!out.wet && world_.raining) {
        const BlockPos at_feet{static_cast<i32>(std::floor(feet.x)),
                               static_cast<i32>(std::floor(feet.y)),
                               static_cast<i32>(std::floor(feet.z))};
        const BlockPos at_head{at_feet.x, static_cast<i32>(std::floor(feet.y + height)), at_feet.z};
        out.wet = is_raining_at(at_feet) || is_raining_at(at_head);
    }
    return out;
}

// ── Mobs ────────────────────────────────────────────────────────────────────

void FireSession::set_mob_on_fire(i32 network_id, i32 seconds) {
    const std::scoped_lock lock{pending_mutex_};
    pending_.push_back(PendingFire{network_id, seconds});
}

void FireSession::hurt_mob(entity::EntityState& state, gameplay::DamageKind kind, f32 amount,
                           bool burning, const FireMobHost& host, FireMobStats& stats) {
    if (mob_combat_ == nullptr || amount <= 0.0F || state.removed) {
        return;
    }
    const MobHurt result = mob_combat_->hurt(state, amount, damage_constants_);
    if (!result.applied) {
        return;
    }
    ++stats.hurt;
    if (host.broadcast) {
        host.broadcast(net::clientbound::kDamageEvent,
                       net::encode_damage_event(state.network_id, damage_type_id(kind),
                                                std::nullopt, std::nullopt));
        net::MetadataWriter fields;
        fields.float_value(net::metadata::kHealth, state.health);
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(state.network_id, fields.take()));
    }
    if (!result.killed) {
        return;
    }
    ++stats.died;
    if (host.broadcast) {
        host.broadcast(net::clientbound::kEntityEvent, net::encode_entity_event(state.network_id, 3));
    }
    std::vector<gameplay::Drop> drops;
    (void)mob_combat_->loot(state, false, 0, loot_random_, drops, burning);
    if (host.drop_item) {
        for (const gameplay::Drop& drop : drops) {
            host.drop_item(Vec3d{state.position.x,
                                 state.position.y + static_cast<f64>(state.height) * 0.5,
                                 state.position.z},
                           net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)), {}});
        }
    }
    state.removed = true;
    mob_combat_->forget(state.network_id);
}

FireMobStats FireSession::tick_mobs(entity::EntityWorld& world, const FireMobHost& host) {
    FireMobStats stats;
    {
        const std::scoped_lock lock{pending_mutex_};
        applying_.swap(pending_);
    }
    for (const PendingFire& request : applying_) {
        gameplay::set_on_fire(mobs_[request.network_id].fire, request.seconds);
    }
    applying_.clear();

    for (const entity::EntityHandle handle : world.handles()) {
        entity::EntityState* state = world.mutable_state(handle);
        // Living things only: a primed TNT, a falling block and an arrow have
        // no health and do not burn here.
        if (state == nullptr || state->removed || state->max_health <= 0.0F) {
            continue;
        }
        const gameplay::FireContact touch =
            contact(state->position, static_cast<f64>(state->width),
                    static_cast<f64>(state->height));
        auto found = mobs_.find(state->network_id);
        const bool sun_sensitive =
            state->type == zombie_type_ || state->type == skeleton_type_;
        const bool hot = touch.in_fire || touch.in_soul_fire || touch.in_lava || touch.campfire != 0;
        if (found == mobs_.end() && !hot && !sun_sensitive) {
            continue;
        }
        if (found == mobs_.end()) {
            found = mobs_.emplace(state->network_id, MobFire{}).first;
        }
        MobFire& mob = found->second;

        const gameplay::FireDamage owed = gameplay::tick_entity_fire(mob.fire, touch, constants_);

        // The sun, after the counter: `Zombie.aiStep` runs after `baseTick`.
        if (sun_sensitive && host_.sky_light && host_.block_light) {
            const BlockPos eyes{static_cast<i32>(std::floor(state->position.x)),
                                static_cast<i32>(std::floor(state->position.y +
                                                            static_cast<f64>(state->eye_height))),
                                static_cast<i32>(std::floor(state->position.z))};
            const u8  sky    = host_.sky_light(eyes);
            const i32 dimmed = std::max(0, static_cast<i32>(sky) - world_.sky_darken);
            const i32 light  = std::max<i32>(dimmed, host_.block_light(eyes));
            if (gameplay::sun_burns(light, world_.sky_darken, sky >= 15, touch.wet, random_)) {
                gameplay::set_on_fire(mob.fire, 8);
                ++stats.sun_lit;
            }
        }

        const bool burning = mob.fire.on_fire();
        if (burning) {
            ++stats.burning;
        }
        hurt_mob(*state, gameplay::DamageKind::OnFire, owed.on_fire, burning, host, stats);
        hurt_mob(*state, gameplay::DamageKind::Lava, owed.lava, burning, host, stats);
        hurt_mob(*state, gameplay::DamageKind::InFire, owed.in_fire, burning, host, stats);

        if (burning != mob.flag_sent && host.broadcast && !state->removed) {
            host.broadcast(net::clientbound::kEntityMetadata,
                           flag_payload(state->network_id, burning, 0));
            mob.flag_sent = burning;
        }
    }

    // Forget what is gone, and what has cooled down.
    gone_.clear();
    for (const auto& [id, mob] : mobs_) {
        const entity::EntityHandle handle = world.find(id);
        const entity::EntityState* state =
            handle == entity::kNoEntity ? nullptr : world.state(handle);
        if (state == nullptr || state->removed ||
            (!mob.fire.on_fire() && !mob.flag_sent && mob.fire.remaining <= -mob.fire.immune_ticks &&
             state->type != zombie_type_ && state->type != skeleton_type_)) {
            gone_.push_back(id);
        }
    }
    for (const i32 id : gone_) {
        mobs_.erase(id);
    }
    return stats;
}

// ── Players ─────────────────────────────────────────────────────────────────

void FireSession::tick_player(gameplay::EntityFire& fire, bool& flag_sent,
                              const gameplay::FireContact& contact_now, bool mortal,
                              bool resistant, const FirePlayerIo& io) {
    if (!mortal) {
        // Creative and spectator: no flames. A counter left burning from
        // survival is put out rather than kept for later.
        fire.remaining = -fire.immune_ticks;
    } else {
        const gameplay::FireDamage owed = gameplay::tick_entity_fire(fire, contact_now, constants_);
        if (!resistant && io.hurt) {
            if (owed.on_fire > 0.0F) {
                (void)io.hurt(gameplay::DamageKind::OnFire, owed.on_fire);
            }
            if (owed.lava > 0.0F) {
                (void)io.hurt(gameplay::DamageKind::Lava, owed.lava);
            }
            if (owed.in_fire > 0.0F) {
                (void)io.hurt(gameplay::DamageKind::InFire, owed.in_fire);
            }
        }
    }
    const bool burning = fire.on_fire();
    if (burning != flag_sent) {
        flag_sent = burning;
        if (io.flag) {
            io.flag(burning);
        }
    }
}

}  // namespace ov::server
