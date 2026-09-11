#define OV_LOG_CATEGORY "weather"

#include "weather_session.hpp"

#include "ov/base/log.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/world/chunk_section.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace ov::server {

namespace {

[[nodiscard]] std::string translate(std::string_view key) {
    std::string json = R"({"translate":")";
    json += key;
    json += R"("})";
    return json;
}

/// A translatable component whose arguments are bare strings — how the game
/// writes `sleep.players_sleeping`: `"with":["1","2"]`, captured.
[[nodiscard]] std::string translate(std::string_view key, i32 first, i32 second) {
    std::string json = R"({"translate":")";
    json += key;
    json += R"(","with":[")";
    json += std::to_string(first);
    json += R"(",")";
    json += std::to_string(second);
    json += R"("]})";
    return json;
}

[[nodiscard]] BlockPos above(BlockPos pos) noexcept { return BlockPos{pos.x, pos.y + 1, pos.z}; }

}  // namespace

WeatherSession::WeatherSession(const registry::BlockRegistry& blocks,
                               const registry::Registries& registries, MobCombat* mob_combat,
                               u64 seed)
    : blocks_{&blocks},
      registries_{&registries},
      mob_combat_{mob_combat},
      precipitation_{blocks, registries},
      beds_{blocks},
      random_{static_cast<i64>(seed)},
      bolt_random_{static_cast<i64>(seed ^ 0x5DEECE66DULL)} {
    if (const auto rod = blocks.find_block("minecraft:lightning_rod")) {
        rod_       = *rod;
        rod_known_ = true;
    }
    entity_types_ = registries.find("minecraft:entity_type");
    if (entity_types_) {
        bolt_type_ = registries.protocol_id(*entity_types_, "minecraft:lightning_bolt").value_or(-1);
    }
    bolts_.reserve(16);
    players_.reserve(64);
    rods_.reserve(64);
    bed_requests_.reserve(8);
    leave_requests_.reserve(8);
    bed_taking_.reserve(8);
    leave_taking_.reserve(8);
    waking_.reserve(8);
    loot_drops_.reserve(16);
}

// ── Any thread ──────────────────────────────────────────────────────────────

void WeatherSession::request_bed(i32 player, BlockPos clicked) {
    const std::scoped_lock lock{requests_mutex_};
    bed_requests_.push_back(BedRequest{player, clicked});
}

void WeatherSession::request_leave(i32 player) {
    const std::scoped_lock lock{requests_mutex_};
    leave_requests_.push_back(player);
}

bool WeatherSession::sleeping(i32 player) const {
    const std::scoped_lock lock{requests_mutex_};
    return std::ranges::find(sleeping_ids_, player) != sleeping_ids_.end();
}

// ── Reading the world ───────────────────────────────────────────────────────

const world::Chunk* WeatherSession::chunk_of(const world::ChunkMap& chunks, BlockPos pos) const {
    return chunks.find(ChunkPos{pos.x >> 4, pos.z >> 4});
}

gameplay::BiomeClimate WeatherSession::climate_at(const world::Chunk& chunk, BlockPos pos) const {
    const world::WorldShape shape = world::WorldShape::overworld();
    const i32               y     = std::clamp(pos.y, shape.min_y, shape.max_y());
    const u16 biome = chunk.get_biome(static_cast<usize>(pos.x & 15), y, static_cast<usize>(pos.z & 15));
    return gameplay::climate_of(blocks_->biome(biome));
}

u8 WeatherSession::light_at(const world::Chunk& chunk, BlockPos pos, bool sky) const {
    const world::ChunkSection* section = chunk.section_for_y(pos.y);
    if (section == nullptr) {
        return sky && pos.y > 0 ? u8{15} : u8{0};
    }
    const usize index = world::section_index(static_cast<usize>(pos.x & 15),
                                             static_cast<usize>(pos.y & 15),
                                             static_cast<usize>(pos.z & 15));
    return sky ? section->sky_light().get(index) : section->block_light().get(index);
}

i32 WeatherSession::surface(const world::Chunk& chunk, i32 x, i32 z, bool motion) const {
    return chunk
        .heightmap(motion ? world::HeightmapType::MotionBlocking : world::HeightmapType::WorldSurface)
        .first_free(static_cast<usize>(x & 15), static_cast<usize>(z & 15));
}

const WeatherPlayer* WeatherSession::find(i32 player) const {
    for (const WeatherPlayer& who : players_) {
        if (who.entity_id == player) {
            return &who;
        }
    }
    return nullptr;
}

bool WeatherSession::is_raining_at(const world::ChunkMap& chunks, BlockPos pos,
                                   const cmd::Weather& weather) const {
    if (!weather.is_raining()) {
        return false;
    }
    const world::Chunk* chunk = chunk_of(chunks, pos);
    if (chunk == nullptr) {
        return false;
    }
    // Sees the sky, nothing that stops rain above it, and a biome where what
    // falls here is rain.
    if (light_at(*chunk, pos, true) < 15 || surface(*chunk, pos.x, pos.z, true) > pos.y) {
        return false;
    }
    return climate_.precipitation_at(climate_at(*chunk, pos), pos) == gameplay::Precipitation::Rain;
}

u8 WeatherSession::sky_darken(const cmd::WorldState& world) noexcept {
    return gameplay::sky_darken(world.day_time, world.weather.rain_level, world.weather.thunder_level);
}

// ── The tick ────────────────────────────────────────────────────────────────

WeatherStats WeatherSession::tick(ServerLevel& level, world::ChunkMap& chunks,
                                  std::span<const ChunkPos> selected, cmd::WorldState& world,
                                  entity::EntityWorld* mobs, const WeatherHost& host) {
    WeatherStats stats;
    players_.clear();
    if (host.players) {
        host.players(players_);
    }

    // A storm is thunder above nine tenths of a sky that is raining.
    const cmd::Weather& weather   = world.weather;
    const bool          raining   = weather.is_raining();
    const bool          thundering = raining && weather.thunder_level * weather.rain_level > 0.9F;
    const i32           snow_height = world.rules.number("snowAccumulationHeight");

    for (const ChunkPos pos : selected) {
        if (chunks.find(pos) == nullptr) {
            continue;
        }
        // The same rule as the random tick: never write into a chunk whose
        // neighbours are not all resident.
        bool surrounded = true;
        for (i32 dz = -1; dz <= 1 && surrounded; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (!chunks.contains(ChunkPos{pos.x + dx, pos.z + dz})) {
                    surrounded = false;
                    break;
                }
            }
        }
        if (!surrounded) {
            continue;
        }
        // Lightning first, then precipitation: the game's order in a chunk.
        if (thundering) {
            lightning(level, chunks, pos, weather, mobs, host, stats);
        }
        precipitation(level, chunks, pos, raining, snow_height, stats);
    }

    tick_bolts(level, mobs, host, world, stats);
    answer_beds(level, world, mobs, host, stats);
    tick_sleepers(level, world, host, stats);

    {
        const std::scoped_lock lock{requests_mutex_};
        sleeping_ids_.clear();
        for (const auto& [id, sleeper] : sleepers_) {
            sleeping_ids_.push_back(id);
        }
    }
    stats.bolts_alive = bolts_.size();
    return stats;
}

void WeatherSession::precipitation(ServerLevel& level, world::ChunkMap& chunks, ChunkPos chunk,
                                   bool raining, i32 snow_height, WeatherStats& stats) {
    // One chunk in sixteen, one column of it.
    if (random_.next_int(16) != 0) {
        return;
    }
    const i32           x     = random_.next_int(16);
    const i32           z     = random_.next_int(16);
    const world::Chunk* found = chunks.find(chunk);
    if (found == nullptr) {
        return;
    }
    const BlockPos top{chunk.x * 16 + x, surface(*found, x, z, true), chunk.z * 16 + z};
    gameplay::PrecipitationColumn column;
    column.top         = top;
    column.biome       = climate_at(*found, top);
    column.light_top   = light_at(*found, top, false);
    column.light_below = light_at(*found, BlockPos{top.x, top.y - 1, top.z}, false);
    column.sea_level   = level.traits().natural ? gameplay::kOverworldSeaLevel : 32;
    precipitation_.tick_column(level, column, climate_, raining, snow_height, random_,
                               stats.precipitation);
    ++stats.columns;
}

void WeatherSession::lightning(ServerLevel& level, world::ChunkMap& chunks, ChunkPos chunk,
                               const cmd::Weather& weather, entity::EntityWorld* mobs,
                               const WeatherHost& host, WeatherStats& stats) {
    if (random_.next_int(thunder_chance_) != 0) {
        return;
    }
    const i32           x     = random_.next_int(16);
    const i32           z     = random_.next_int(16);
    const world::Chunk* found = chunks.find(chunk);
    if (found == nullptr) {
        return;
    }
    const BlockPos top{chunk.x * 16 + x, surface(*found, x, z, true), chunk.z * 16 + z};
    BlockPos       target = top;

    // A lightning rod within range, the highest block of its column. Only the
    // top of each column can hold one that counts, so only the top is read.
    rods_.clear();
    if (rod_known_) {
        constexpr i32 kReachChunks = (gameplay::kLightningRodRange >> 4) + 1;
        for (i32 cz = (top.z >> 4) - kReachChunks; cz <= (top.z >> 4) + kReachChunks; ++cz) {
            for (i32 cx = (top.x >> 4) - kReachChunks; cx <= (top.x >> 4) + kReachChunks; ++cx) {
                const world::Chunk* other = chunks.find(ChunkPos{cx, cz});
                if (other == nullptr) {
                    continue;
                }
                for (i32 lz = 0; lz < 16; ++lz) {
                    for (i32 lx = 0; lx < 16; ++lx) {
                        const i32 y = surface(*other, lx, lz, false) - 1;
                        if (!level.shape().contains_y(y)) {
                            continue;
                        }
                        const registry::BlockStateId state = other->get_block(
                            static_cast<usize>(lx), y, static_cast<usize>(lz));
                        if (blocks_->block_of(state) == rod_) {
                            rods_.push_back(BlockPos{cx * 16 + lx, y, cz * 16 + lz});
                        }
                    }
                }
            }
        }
    }
    if (const auto rod = gameplay::nearest_rod(top, rods_)) {
        target = above(*rod);
        ++stats.rods;
    } else {
        // A living thing in the column that sees the sky, chosen at random.
        const AABB box = gameplay::lightning_target_box(top, level.shape().max_y() + 1);
        std::vector<Candidate>& candidates = candidates_;
        candidates.clear();
        const auto sees_sky = [&](const Vec3d& feet) {
            const BlockPos cell{static_cast<i32>(std::floor(feet.x)), static_cast<i32>(std::floor(feet.y)),
                                static_cast<i32>(std::floor(feet.z))};
            const world::Chunk* in = chunk_of(chunks, cell);
            return in != nullptr && light_at(*in, cell, true) >= 15;
        };
        for (const WeatherPlayer& who : players_) {
            if (!who.spectator && who.alive &&
                box.intersects(AABB::from_entity(who.feet, 0.6, 1.8)) && sees_sky(who.feet)) {
                candidates.push_back(Candidate{who.feet});
            }
        }
        if (mobs != nullptr) {
            for (const entity::EntityHandle handle : mobs->handles()) {
                const entity::EntityState* state = mobs->state(handle);
                if (state == nullptr || state->removed || state->max_health <= 0.0F ||
                    state->health <= 0.0F) {
                    continue;
                }
                if (box.intersects(AABB::from_entity(state->position, static_cast<f64>(state->width),
                                      static_cast<f64>(state->height))) &&
                    sees_sky(state->position)) {
                    candidates.push_back(Candidate{state->position});
                }
            }
        }
        if (!candidates.empty()) {
            const i32   pick = random_.next_int(static_cast<i32>(candidates.size()));
            const Vec3d at   = candidates[static_cast<usize>(pick)].at;
            target = BlockPos{static_cast<i32>(std::floor(at.x)), static_cast<i32>(std::floor(at.y)),
                              static_cast<i32>(std::floor(at.z))};
        }
    }

    if (!is_raining_at(chunks, target, weather)) {
        return;
    }
    // The skeleton trap: a chance of `local difficulty × 1 %` of a skeleton
    // horse trap instead of a plain bolt. Local difficulty and the trap horse
    // are not modelled; the bolt still falls, and that is said once.
    if (!horse_named_) {
        OV_LOG_INFO("lightning: the skeleton horse trap is not modelled; every bolt is a plain one");
        horse_named_ = true;
    }
    if (bolt_type_ < 0 || !host.allocate_entity_id) {
        return;
    }
    Bolt bolt;
    bolt.id    = host.allocate_entity_id();
    bolt.at    = Vec3d{static_cast<f64>(target.x) + 0.5, static_cast<f64>(target.y),
                    static_cast<f64>(target.z) + 0.5};
    bolt.clock = gameplay::new_bolt(bolt_random_);
    net::SpawnEntity spawn;
    spawn.entity_id = bolt.id;
    spawn.uuid      = net::Uuid{0x4F56424F4C540000ULL | static_cast<u64>(static_cast<u32>(bolt.id)),
                           static_cast<u64>(bolt.id) * 0x9E3779B97F4A7C15ULL};
    spawn.type = bolt_type_;
    spawn.x    = bolt.at.x;
    spawn.y    = bolt.at.y;
    spawn.z    = bolt.at.z;
    if (host.broadcast) {
        host.broadcast(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    }
    bolts_.push_back(std::move(bolt));
    ++stats.strikes;
}

// ── Bolts ───────────────────────────────────────────────────────────────────

void WeatherSession::tick_bolts(ServerLevel& level, entity::EntityWorld* mobs,
                                const WeatherHost& host, const cmd::WorldState& world,
                                WeatherStats& stats) {
    for (usize i = 0; i < bolts_.size();) {
        Bolt&                  bolt = bolts_[i];
        const gameplay::BoltStep step = gameplay::tick_bolt(bolt.clock, bolt_random_);
        if ((step.strike || step.refire) && world.difficulty >= 2 &&
            world.rules.flag("doFireTick")) {
            // Fire at the strike and at every later flash — on Normal and
            // Hard with doFireTick: the struck block, and on the first flash
            // up to four more, each at a random offset of -1..1 on every axis.
            // Where a fire may not stand, nothing is written (`ignite`).
            const BlockPos at{static_cast<i32>(std::floor(bolt.at.x)),
                              static_cast<i32>(std::floor(bolt.at.y)),
                              static_cast<i32>(std::floor(bolt.at.z))};
            if (host.ignite) {
                (void)host.ignite(at);
                for (i32 extra = 0; step.strike && extra < 4; ++extra) {
                    const i32 dx = bolt_random_.next_int(3) - 1;
                    const i32 dy = bolt_random_.next_int(3) - 1;
                    const i32 dz = bolt_random_.next_int(3) - 1;
                    (void)host.ignite(BlockPos{at.x + dx, at.y + dy, at.z + dz});
                }
            } else if (!fire_named_) {
                OV_LOG_INFO("lightning: no fire on this server; bolts set nothing alight");
                fire_named_ = true;
            }
        }
        if (step.strike) {
            // A struck rod is powered for eight ticks. The redstone engine
            // does not know the rod as a source yet: the state is not written,
            // and that is said once.
            const BlockPos below{static_cast<i32>(std::floor(bolt.at.x)),
                                 static_cast<i32>(std::floor(bolt.at.y)) - 1,
                                 static_cast<i32>(std::floor(bolt.at.z))};
            if (rod_known_ && blocks_->block_of(level.block_at(below)) == rod_ && !rod_named_) {
                OV_LOG_INFO("lightning: a struck rod's redstone pulse is not modelled");
                rod_named_ = true;
            }
        }
        if (step.lit) {
            strike(bolt, level, mobs, host, world, stats);
        }
        if (step.remove) {
            if (host.broadcast) {
                host.broadcast(net::clientbound::kRemoveEntities, net::encode_remove_entity(bolt.id));
            }
            bolts_.erase(bolts_.begin() + static_cast<isize>(i));
            continue;
        }
        ++i;
    }
}

void WeatherSession::strike(Bolt& bolt, ServerLevel& level, entity::EntityWorld* mobs,
                            const WeatherHost& host, const cmd::WorldState& world,
                            WeatherStats& stats) {
    (void)level;
    const AABB box      = gameplay::lightning_hit_box(bolt.at);
    const auto was_hit  = [&](i32 id) {
        return std::ranges::find(bolt.hit, id) != bolt.hit.end();
    };
    // Every lit tick strikes again — measured: a NoAI zombie under one bolt
    // went 20 → 15.06 → 10.14 → 5.22, one hit a flash. The damage window
    // decides what each later hit is worth; a conversion happens once.
    for (const WeatherPlayer& who : players_) {
        if (who.spectator || !who.alive ||
            !box.intersects(AABB::from_entity(who.feet, 0.6, 1.8))) {
            continue;
        }
        if (!who.creative && host.hurt_player) {
            host.hurt_player(who.entity_id, gameplay::kLightningDamage);
            ++stats.hurt;
        }
    }

    if (mobs != nullptr && entity_types_) {
        const bool peaceful = world.difficulty == 0;
        for (const entity::EntityHandle handle : mobs->handles()) {
            entity::EntityState* state = mobs->mutable_state(handle);
            if (state == nullptr || state->removed || state->max_health <= 0.0F ||
                !box.intersects(AABB::from_entity(state->position, static_cast<f64>(state->width),
                                      static_cast<f64>(state->height)))) {
                continue;
            }
            const bool first = !was_hit(state->network_id);
            if (first) {
                bolt.hit.push_back(state->network_id);
            }
            const std::string_view type = registries_->entry_of(*entity_types_, state->type);
            switch (first ? gameplay::lightning_conversion(type) : gameplay::LightningConversion::None) {
            case gameplay::LightningConversion::ZombifiedPiglin:
            case gameplay::LightningConversion::Witch:
                if (!peaceful && host.convert_mob) {
                    const auto into = gameplay::lightning_conversion(type) ==
                                              gameplay::LightningConversion::Witch
                                          ? std::string_view{"minecraft:witch"}
                                          : std::string_view{"minecraft:zombified_piglin"};
                    if (host.convert_mob(handle, into)) {
                        ++stats.converted;
                        continue;
                    }
                }
                break;
            case gameplay::LightningConversion::ChargeCreeper:
                if (host.charge_creeper) {
                    host.charge_creeper(state->network_id);
                    ++stats.converted;
                }
                break;
            case gameplay::LightningConversion::FlipMooshroom:
                // Red and brown swap, and no damage is done. This server
                // carries no mooshroom variant: nothing to swap, said once.
                if (!mooshroom_named_) {
                    OV_LOG_INFO("lightning: the mooshroom's red/brown swap is not modelled");
                    mooshroom_named_ = true;
                }
                continue;
            case gameplay::LightningConversion::Kill:
            case gameplay::LightningConversion::None:
                break;
            }
            if (mob_combat_ == nullptr) {
                continue;
            }
            const f32 amount = gameplay::lightning_conversion(type) == gameplay::LightningConversion::Kill
                                   ? 1.0e9F
                                   : gameplay::kLightningDamage;
            const MobHurt hurt = mob_combat_->hurt(*state, amount, damage_constants_);
            if (hurt.applied) {
                ++stats.hurt;
            }
            if (hurt.killed) {
                loot_drops_.clear();
                (void)mob_combat_->loot(*state, false, 0, loot_random_, loot_drops_);
                if (host.drop_item) {
                    for (const gameplay::Drop& drop : loot_drops_) {
                        host.drop_item(state->position,
                                       net::ItemStack{drop.item,
                                                      static_cast<i8>(std::min(drop.count, 64)), {}});
                    }
                }
                state->removed = true;
            }
        }
    }

    // A dropped stack has five health: a bolt burns it.
    if (host.sweep_items) {
        host.sweep_items([&](Vec3d at) { return box.contains(at); });
    }
}

// ── Beds ────────────────────────────────────────────────────────────────────

void WeatherSession::message(const WeatherHost& host, i32 player, std::string_view key,
                             bool overlay) const {
    if (host.send_to) {
        host.send_to(player, net::clientbound::kSystemChat,
                     net::encode_system_chat(translate(key), overlay));
    }
}

bool WeatherSession::monsters_near(const AABB& box, entity::EntityWorld* mobs) const {
    if (mobs == nullptr || !entity_types_) {
        return false;
    }
    for (const entity::EntityHandle handle : mobs->handles()) {
        const entity::EntityState* state = mobs->state(handle);
        if (state == nullptr || state->removed || state->health <= 0.0F) {
            continue;
        }
        if (gameplay::prevents_rest(registries_->entry_of(*entity_types_, state->type)) &&
            box.intersects(AABB::from_entity(state->position, static_cast<f64>(state->width),
                                      static_cast<f64>(state->height)))) {
            return true;
        }
    }
    return false;
}

void WeatherSession::answer_beds(ServerLevel& level, cmd::WorldState& world,
                                 entity::EntityWorld* mobs, const WeatherHost& host,
                                 WeatherStats& stats) {
    {
        const std::scoped_lock lock{requests_mutex_};
        bed_taking_.swap(bed_requests_);
        leave_taking_.swap(leave_requests_);
    }
    const i32 percentage = world.rules.number("playersSleepingPercentage");
    for (const BedRequest& request : bed_taking_) {
        const WeatherPlayer* who = find(request.player);
        if (who == nullptr) {
            continue;
        }
        const auto bed = beds_.bed_at(level, request.clicked);
        if (!bed) {
            continue;
        }
        gameplay::SleepCheck check;
        check.bed_works        = level.traits().natural;
        check.natural          = level.traits().natural;
        check.occupied         = bed->occupied;
        check.sleeping_or_dead = sleepers_.contains(who->entity_id) || !who->alive;
        check.in_range         = gameplay::BedRules::in_range(who->feet, *bed);
        check.obstructed       = beds_.obstructed(level, *bed);
        check.is_day           = gameplay::is_day(sky_darken(world));
        check.creative         = who->creative;
        check.monsters_near    = monsters_near(gameplay::BedRules::monster_box(*bed), mobs);
        const gameplay::SleepVerdict verdict = gameplay::judge_sleep(check);

        if (verdict.explodes) {
            // Both halves go, then an explosion of five with fire from the
            // head's centre.
            level.set_block(bed->head, registry::kAirState);
            level.set_block(bed->foot, registry::kAirState);
            const Vec3d centre{static_cast<f64>(bed->head.x) + 0.5,
                               static_cast<f64>(bed->head.y) + 0.5,
                               static_cast<f64>(bed->head.z) + 0.5};
            if (host.explode) {
                host.explode(centre, gameplay::kBedExplosionPower, true);
            } else {
                OV_LOG_WARN("a bed exploded where beds do not work, and this server has no "
                            "explosion to give it");
            }
            continue;
        }
        if (verdict.sets_spawn && host.set_spawn) {
            if (host.set_spawn(who->uuid, bed->head, who->yaw)) {
                message(host, who->entity_id, "block.minecraft.set_spawn", false);
            }
            bed_spawns_[who->uuid.to_string()] = bed->head;
        }
        if (verdict.problem != gameplay::BedProblem::None) {
            const std::string_view key = gameplay::bed_message_key(verdict.problem);
            if (!key.empty()) {
                message(host, who->entity_id, key, true);
            }
            continue;
        }
        lie_down(level, *who, *bed, host);
        if (percentage > 100) {
            message(host, who->entity_id, "sleep.not_possible", true);
        }
        announce(host, percentage);
        ++stats.slept;
    }
    bed_taking_.clear();
}

void WeatherSession::lie_down(ServerLevel& level, const WeatherPlayer& who, const gameplay::Bed& bed,
                              const WeatherHost& host) {
    level.set_block(bed.head, beds_.with_occupied(level.block_at(bed.head), true));
    level.set_block(bed.foot, beds_.with_occupied(level.block_at(bed.foot), true));
    sleepers_[who.entity_id] = Sleeper{bed.head, 0};
    if (host.place_player) {
        host.place_player(who.entity_id, gameplay::BedRules::sleeping_position(bed), who.yaw,
                          who.pitch, false);
    }
    net::MetadataWriter fields;
    fields.pose_value(kPoseIndex, kPoseSleeping);
    fields.optional_block_pos_value(kSleepingPos,
                                    net::WirePosition{bed.head.x, bed.head.y, bed.head.z});
    if (host.broadcast) {
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(who.entity_id, fields.take()));
    }
}

void WeatherSession::wake(ServerLevel& level, i32 player, bool animate, bool announce_change,
                          const WeatherHost& host, i32 percentage) {
    const auto it = sleepers_.find(player);
    if (it == sleepers_.end()) {
        return;
    }
    const BlockPos head = it->second.head;
    sleepers_.erase(it);
    const WeatherPlayer* who = find(player);

    if (animate && host.broadcast) {
        host.broadcast(net::clientbound::kEntityAnimation,
                       net::encode_entity_animation(player, kAnimationWake));
    }
    const auto bed = beds_.bed_at(level, head);
    if (bed) {
        level.set_block(bed->head, beds_.with_occupied(level.block_at(bed->head), false));
        level.set_block(bed->foot, beds_.with_occupied(level.block_at(bed->foot), false));
        const f32   yaw   = who != nullptr ? who->yaw : 0.0F;
        const Vec3d stand = beds_.stand_up_position(level, *bed, yaw)
                                .value_or(Vec3d{static_cast<f64>(head.x) + 0.5,
                                                static_cast<f64>(head.y) + 1.1,
                                                static_cast<f64>(head.z) + 0.5});
        if (host.place_player) {
            host.place_player(player, stand, gameplay::BedRules::stand_up_yaw(*bed, stand), 0.0F,
                              true);
        }
    } else if (who != nullptr && host.place_player) {
        host.place_player(player, who->feet, who->yaw, who->pitch, animate);
    }
    net::MetadataWriter fields;
    fields.pose_value(kPoseIndex, kPoseStanding);
    fields.optional_block_pos_value(kSleepingPos, std::nullopt);
    if (host.broadcast) {
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(player, fields.take()));
    }
    if (announce_change) {
        announce(host, percentage);
    }
}

void WeatherSession::announce(const WeatherHost& host, i32 percentage) {
    // `canSleepThroughNights`: a percentage above a hundred never skips, and
    // the count is not announced.
    if (!announce_ || percentage > 100) {
        return;
    }
    i32 active   = 0;
    i32 sleeping = 0;
    for (const WeatherPlayer& who : players_) {
        if (who.spectator) {
            continue;
        }
        ++active;
        sleeping += sleepers_.contains(who.entity_id) ? 1 : 0;
    }
    const bool changed =
        (last_sleeping_ > 0 || sleeping > 0) && (last_active_ != active || last_sleeping_ != sleeping);
    last_active_   = active;
    last_sleeping_ = sleeping;
    if (!changed || !host.send_to) {
        return;
    }
    const i32         needed = gameplay::sleepers_needed(active, percentage);
    const std::string json   = sleeping >= needed
                                   ? translate("sleep.skipping_night")
                                   : translate("sleep.players_sleeping", sleeping, needed);
    const std::vector<u8> packet = net::encode_system_chat(json, true);
    for (const WeatherPlayer& who : players_) {
        host.send_to(who.entity_id, net::clientbound::kSystemChat, packet);
    }
}

void WeatherSession::tick_sleepers(ServerLevel& level, cmd::WorldState& world,
                                   const WeatherHost& host, WeatherStats& stats) {
    const i32 percentage = world.rules.number("playersSleepingPercentage");

    for (const i32 id : leave_taking_) {
        if (sleepers_.contains(id)) {
            wake(level, id, true, true, host, percentage);
            ++stats.woke;
        }
    }
    leave_taking_.clear();

    // Gone players free their bed.
    waking_.clear();
    for (const auto& [id, sleeper] : sleepers_) {
        if (find(id) == nullptr) {
            waking_.push_back(id);
        }
    }
    for (const i32 id : waking_) {
        wake(level, id, false, false, host, percentage);
    }

    // Each sleeper: the counter, the day, the bed.
    const bool day = gameplay::is_day(sky_darken(world));
    waking_.clear();
    for (auto& [id, sleeper] : sleepers_) {
        sleeper.counter = std::min(sleeper.counter + 1, gameplay::kSleepTicksNeeded);
        if (day || !beds_.bed_at(level, sleeper.head)) {
            waking_.push_back(id);
        }
    }
    for (const i32 id : waking_) {
        const bool bed_there = sleepers_.contains(id) && beds_.bed_at(level, sleepers_[id].head).has_value();
        // A sleeper whose bed went simply stops sleeping; one woken by the
        // day gets up, is seen getting up, and the count is announced.
        wake(level, id, bed_there, bed_there, host, percentage);
        ++stats.woke;
    }

    // The night skip.
    if (sleepers_.empty() || percentage > 100) {
        return;
    }
    i32 active   = 0;
    i32 sleeping = 0;
    i32 deep     = 0;
    for (const WeatherPlayer& who : players_) {
        if (who.spectator) {
            continue;
        }
        ++active;
        if (const auto it = sleepers_.find(who.entity_id); it != sleepers_.end()) {
            ++sleeping;
            deep += it->second.counter >= gameplay::kSleepTicksNeeded ? 1 : 0;
        }
    }
    const i32 needed = gameplay::sleepers_needed(active, percentage);
    if (sleeping < needed || deep < needed) {
        return;
    }
    if (world.rules.flag("doDaylightCycle")) {
        world.day_time = gameplay::wake_day_time(world.day_time);
    }
    waking_.clear();
    for (const auto& [id, sleeper] : sleepers_) {
        waking_.push_back(id);
    }
    for (const i32 id : waking_) {
        wake(level, id, true, false, host, percentage);
        ++stats.woke;
    }
    last_sleeping_ = 0;
    if (world.rules.flag("doWeatherCycle") && world.weather.is_raining()) {
        world.weather.rain_time    = 0;
        world.weather.raining      = false;
        world.weather.thunder_time = 0;
        world.weather.thundering   = false;
    }
    stats.night_skipped = true;
}

// ── Respawn ─────────────────────────────────────────────────────────────────

std::optional<Vec3d> WeatherSession::bed_respawn(const world::LevelView& level, BlockPos head,
                                                 f32 yaw) const {
    const auto bed = beds_.bed_at(level, head);
    if (!bed) {
        return std::nullopt;
    }
    return beds_.stand_up_position(level, *bed, yaw);
}

bool WeatherSession::spawn_is_bed(const net::Uuid& uuid, BlockPos point) const {
    const auto it = bed_spawns_.find(uuid.to_string());
    return it != bed_spawns_.end() && it->second == point;
}

void WeatherSession::forget(ServerLevel& level, i32 player) {
    if (!sleepers_.contains(player)) {
        return;
    }
    WeatherHost none;
    wake(level, player, false, false, none, 100);
}

void WeatherSession::join_packets(const WeatherDeliver& deliver) const {
    for (const auto& [id, sleeper] : sleepers_) {
        net::MetadataWriter fields;
        fields.pose_value(kPoseIndex, kPoseSleeping);
        fields.optional_block_pos_value(
            kSleepingPos, net::WirePosition{sleeper.head.x, sleeper.head.y, sleeper.head.z});
        deliver(net::clientbound::kEntityMetadata, net::encode_entity_metadata(id, fields.take()));
    }
}

}  // namespace ov::server
