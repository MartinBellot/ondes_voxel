#define OV_LOG_CATEGORY "server"

#include "tnt_gravity.hpp"

#include "entity_nbt.hpp"  // ── persistence ──
#include "survival_session.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/entity_physics.hpp"
#include "ov/protocol/blast.hpp"
#include "ov/protocol/entity.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ov::server {
namespace {

/// A uuid from the wire id, never from a clock: the same SplitMix64 mixing the
/// server gives its mobs, so a replay produces the same bytes.
[[nodiscard]] net::Uuid uuid_for(i32 network_id) noexcept {
    const auto mix = [](u64 z) {
        z += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    const auto id = static_cast<u64>(static_cast<u32>(network_id)) ^ 0x7E57'0000'0000ULL;
    return net::Uuid{mix(id), mix(id ^ 0xA5A5A5A5A5A5A5A5ULL)};
}

/// A player is told about an explosion within this distance of it, squared.
///
/// Sixty-four blocks, from the wiki's Explosion article; not measured here.
/// A player further away sees nothing but the block updates.
constexpr f64 kExplosionAudienceSq = 64.0 * 64.0;

/// A dropped item's health. One explosion hit of five or more removes it.
///
/// From the wiki's Item (entity) article; not measured here. What it does is
/// empty a floor covered in loot, which is visible and is what vanilla does.
constexpr f32 kItemHealth = 5.0F;

/// The eye height of a player, the point an explosion pushes one from.
constexpr f64 kPlayerEye = 1.62;

[[nodiscard]] Vec3d block_centre(BlockPos pos) noexcept {
    return Vec3d{static_cast<f64>(pos.x) + 0.5, static_cast<f64>(pos.y) + 0.5,
                 static_cast<f64>(pos.z) + 0.5};
}

}  // namespace

TntGravity::TntGravity(const registry::BlockRegistry& blocks,
                       const registry::Registries& registries, const gameplay::LootTables* loot,
                       MobCombat* mob_combat)
    : blocks_{&blocks},
      registries_{&registries},
      loot_{loot},
      mob_combat_{mob_combat},
      explosions_{blocks},
      falling_{blocks, registries} {
    if (const auto types = registries.find("minecraft:entity_type")) {
        tnt_type_     = registries.protocol_id(*types, "minecraft:tnt").value_or(-1);
        falling_type_ = registries.protocol_id(*types, "minecraft:falling_block").value_or(-1);
        creeper_type_ = registries.protocol_id(*types, "minecraft:creeper").value_or(-1);
        if (const auto item = registries.protocol_id(*types, "minecraft:item")) {
            if (const auto info = registries.entity_type(*item)) {
                item_eye_ = static_cast<f64>(info->eye_height);
            }
        }
    }
    if (const auto items = registries.find("minecraft:item")) {
        item_registry_       = *items;
        item_registry_valid_ = 1;
    }
    tnt_block_ = blocks.find_block("minecraft:tnt").value_or(registry::BlockId{0});

    // Named once at start-up rather than at every landing: the list is what a
    // player of this server should know does not behave fully yet.
    for (const std::string_view name : gameplay::FallingBlocks::unimplemented()) {
        OV_LOG_DEBUG("falling block {} falls and lands; its other behaviour is not carried out",
                     name);
    }

    pending_falls_.reserve(64);
    pending_primes_.reserve(64);
    spawning_falls_.reserve(64);
    spawning_primes_.reserve(64);
    targets_.reserve(16);
}

// ── The world-tick drain ────────────────────────────────────────────────────

void TntGravity::neighbour_changed(ServerLevel& level, BlockPos pos) {
    const registry::BlockStateId state = level.block_at(pos);
    const registry::BlockId      block = blocks_->block_of(state);
    if (block == tnt_block_ && tnt_block_.value() != 0) {
        // Vanilla's rule: a TNT block with any neighbour signal is primed on
        // the spot. Measured with a redstone block set beside it.
        if (redstone_ != nullptr &&
            gameplay::tnt_lit_by_signal(redstone_->signals(), level, pos, tnt_block_)) {
            level.set_block(pos, registry::kAirState);
            const std::scoped_lock lock{pending_mutex_};
            pending_primes_.push_back(PendingPrime{pos, gameplay::kTntFuseTicks});
        }
        return;
    }
    if (falling_.falls(state)) {
        (void)falling_.neighbour_changed(level, pos);
    }
}

bool TntGravity::scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) {
    const auto block = blocks_->find_block(what);
    if (!block || !falling_.falls(blocks_->default_state(*block))) {
        return false;
    }
    if (const auto start = falling_.tick(level, pos, what)) {
        const std::scoped_lock lock{pending_mutex_};
        pending_falls_.push_back(*start);
    }
    return true;
}

// ── Spawning ────────────────────────────────────────────────────────────────

void TntGravity::request_prime(BlockPos pos, i32 fuse) {
    const std::scoped_lock lock{pending_mutex_};
    pending_primes_.push_back(PendingPrime{pos, fuse});
}

bool TntGravity::has_pending() const {
    const std::scoped_lock lock{pending_mutex_};
    return !pending_falls_.empty() || !pending_primes_.empty();
}

void TntGravity::spawn_pending(entity::EntityWorld& world, const Deliver& deliver) {
    {
        const std::scoped_lock lock{pending_mutex_};
        spawning_falls_.swap(pending_falls_);
        spawning_primes_.swap(pending_primes_);
    }
    for (const gameplay::FallStart& start : spawning_falls_) {
        spawn_falling(world, start, deliver);
    }
    for (const PendingPrime& prime : spawning_primes_) {
        spawn_primed(world, prime.pos, prime.fuse, deliver);
    }
    spawning_falls_.clear();
    spawning_primes_.clear();
}

void TntGravity::spawn_primed(entity::EntityWorld& world, BlockPos pos, i32 fuse,
                              const Deliver& deliver) {
    if (tnt_type_ < 0) {
        return;
    }
    // The bottom centre of the block: captured, a TNT primed in block
    // (16, -57, 5) spawned at (16.5, -57.0, 5.5).
    const Vec3d at{static_cast<f64>(pos.x) + 0.5, static_cast<f64>(pos.y),
                   static_cast<f64>(pos.z) + 0.5};
    const auto  handle = world.spawn(tnt_type_, at, net::Uuid{});
    if (!handle) {
        OV_LOG_WARN("cannot spawn a primed TNT: {}", entity::to_string(handle.error()));
        return;
    }
    entity::EntityState* state = world.mutable_state(*handle);
    state->uuid                = uuid_for(state->network_id);
    state->velocity            = gameplay::tnt_prime_velocity(random_);
    state->broadcast_position  = state->position;
    state->broadcast_valid     = true;
    world.set_logic(*handle, std::make_unique<gameplay::PrimedTntLogic>(fuse, blast_events_));
    spawn_packets(world, *state, deliver);
    if (on_primed_) {  // ── sound ──
        on_primed_(at);
    }
}

void TntGravity::spawn_falling(entity::EntityWorld& world, const gameplay::FallStart& start,
                               const Deliver& deliver) {
    if (falling_type_ < 0) {
        return;
    }
    // Captured: sand set at (19, -54, 5) fell from (19.5, -54.0, 5.5).
    const Vec3d at{static_cast<f64>(start.pos.x) + 0.5, static_cast<f64>(start.pos.y),
                   static_cast<f64>(start.pos.z) + 0.5};
    const auto  handle = world.spawn(falling_type_, at, net::Uuid{});
    if (!handle) {
        OV_LOG_WARN("cannot spawn a falling block: {}", entity::to_string(handle.error()));
        return;
    }
    entity::EntityState* state = world.mutable_state(*handle);
    state->uuid                = uuid_for(state->network_id);
    state->broadcast_position  = state->position;
    state->broadcast_valid     = true;
    world.set_logic(*handle, std::make_unique<gameplay::FallingBlockLogic>(
                                 falling_, start.state, start.pos, falling_events_));
    spawn_packets(world, *state, deliver);
}

void TntGravity::spawn_packets(entity::EntityWorld& world, const entity::EntityState& state,
                               const Deliver& deliver) const {
    net::SpawnEntity spawn;
    spawn.entity_id = state.network_id;
    spawn.uuid      = state.uuid;
    spawn.type      = state.type;
    // The copy the other clients already hold, as mob_packets does, so that a
    // late joiner and an early one agree on where it is.
    const Vec3d anchor = state.broadcast_valid ? state.broadcast_position : state.position;
    spawn.x            = anchor.x;
    spawn.y            = anchor.y;
    spawn.z            = anchor.z;
    spawn.velocity_x   = state.velocity.x;
    spawn.velocity_y   = state.velocity.y;
    spawn.velocity_z   = state.velocity.z;

    entity::IEntityLogic* logic = world.logic(world.find(state.network_id));
    net::MetadataWriter   fields;
    if (state.type == falling_type_) {
        const auto* falling = dynamic_cast<const gameplay::FallingBlockLogic*>(logic);
        if (falling == nullptr) {
            // A falling block with no state would render as air. Refused and
            // named rather than sent as nothing.
            OV_LOG_WARN("falling block {} has no block state to announce", state.network_id);
            return;
        }
        // The `data` field is the block state: captured as 112 for sand and
        // 117 for red sand, both our own ids for those states.
        spawn.data = static_cast<i32>(falling->state().value());
        fields.block_pos_value(net::metadata::kFallingBlockStart,
                               net::WirePosition{falling->start().x, falling->start().y,
                                                 falling->start().z});
    } else if (state.type == tnt_type_) {
        if (const auto* tnt = dynamic_cast<const gameplay::PrimedTntLogic*>(logic);
            tnt != nullptr && tnt->fuse() != net::kDefaultTntFuse) {
            fields.varint_value(net::metadata::kTntFuse, tnt->fuse());
        }
    }
    deliver(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    if (!fields.empty()) {
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state.network_id, fields.take()));
    }
}

// ── Creepers ────────────────────────────────────────────────────────────────

void TntGravity::tick_creepers(entity::EntityWorld& world, const world::LevelView& level,
                               const TntGravityHost& host, const Deliver& deliver) {
    if (creeper_type_ < 0) {
        return;
    }
    // Forget the ones that are gone. A handful of entries, once a tick.
    std::erase_if(creepers_, [&](const auto& entry) {
        return world.find(entry.first) == entity::kNoEntity;
    });

    bool any = false;
    for (const entity::EntityHandle handle : world.handles()) {
        const entity::EntityState* state = world.state(handle);
        if (state != nullptr && state->type == creeper_type_ && !state->removed) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    targets_.clear();
    if (host.creeper_targets) {
        host.creeper_targets(targets_);
    }

    for (const entity::EntityHandle handle : world.handles()) {
        entity::EntityState* state = world.mutable_state(handle);
        if (state == nullptr || state->type != creeper_type_ || state->removed) {
            continue;
        }
        gameplay::CreeperSwell& swell = creepers_[state->network_id];

        std::optional<f64> nearest;
        bool               sees = false;
        const Vec3d        eye{state->position.x,
                        state->position.y + static_cast<f64>(state->eye_height),
                        state->position.z};
        for (const Vec3d& feet : targets_) {
            const f64 d = (feet - state->position).length();
            if (!nearest || d < *nearest) {
                nearest = d;
                const Vec3d their_eye{feet.x, feet.y + kPlayerEye, feet.z};
                sees = !explosions_.clipped(level, eye, their_eye);
            }
        }

        const gameplay::CreeperStep step = gameplay::tick_creeper(swell, nearest, sees);
        if (step.direction_changed) {
            net::MetadataWriter fields;
            fields.varint_value(net::metadata::kCreeperSwellDir, swell.direction);
            deliver(net::clientbound::kEntityMetadata,
                    net::encode_entity_metadata(state->network_id, fields.take()));
        }
        if (step.explode) {
            // From the creeper's feet. A creeper is removed by its own
            // explosion, not killed: nothing it would have dropped drops.
            blast_events_.blasts.push_back(gameplay::BlastEvents::Blast{
                state->position, gameplay::creeper_power(swell), gameplay::kCreeperInteraction,
                state->network_id});
            state->removed = true;
        }
    }
}

// ── After the entities ──────────────────────────────────────────────────────

TntGravityStats TntGravity::after_entity_tick(entity::EntityWorld& world, ServerLevel& level,
                                              WorldTicks& ticks, const TntGravityHost& host,
                                              const Deliver& deliver) {
    TntGravityStats stats;

    for (const gameplay::FallingEvents::Landed& landed : falling_events_.landed) {
        ++stats.landed;
        // ── persistence ── a falling block read with `DropItem:0b` drops nothing
        const auto kept     = saved_.find(landed.network_id);
        const bool may_drop = kept == saved_.end() || get_bool(kept->second, "DropItem", true);
        if (landed.expired) {
            if (may_drop) {
                drop_block_item(landed.state, landed.cell, host);
                ++stats.dropped;
            }
            continue;
        }
        const gameplay::Landing result =
            falling_.land(level, landed.cell, landed.state, landed.in_water);
        if (result == gameplay::Landing::Dropped && may_drop) {
            drop_block_item(landed.state, landed.cell, host);
            ++stats.dropped;
        }
    }
    falling_events_.landed.clear();
    for (const i32 gone : world.removed_ids()) {  // ── persistence ──
        saved_.erase(gone);
    }

    // The fuse, every tick, for every TNT still burning. Measured: vanilla
    // re-sends index 8 on each tick of the countdown.
    if (tnt_type_ >= 0) {
        for (const entity::EntityHandle handle : world.handles()) {
            const entity::EntityState* state = world.state(handle);
            if (state == nullptr || state->type != tnt_type_ || state->removed) {
                continue;
            }
            if (const auto* tnt = dynamic_cast<const gameplay::PrimedTntLogic*>(world.logic(handle))) {
                net::MetadataWriter fields;
                fields.varint_value(net::metadata::kTntFuse, tnt->fuse());
                deliver(net::clientbound::kEntityMetadata,
                        net::encode_entity_metadata(state->network_id, fields.take()));
            }
        }
    }

    // By index: a blast never adds a blast in the same tick (a chained TNT
    // waits at least ten), but indexing keeps that an observation rather
    // than an iterator invalidation waiting to happen.
    for (usize i = 0; i < blast_events_.blasts.size(); ++i) {
        const gameplay::BlastEvents::Blast blast = blast_events_.blasts[i];
        detonate(world, level, blast, host, deliver, stats);
    }
    blast_events_.blasts.clear();

    if (stats.blasts > 0 || stats.landed > 0) {
        (void)ticks.settle_changes(level);
    }
    return stats;
}

void TntGravity::detonate(entity::EntityWorld& world, ServerLevel& level,
                          const gameplay::BlastEvents::Blast& blast, const TntGravityHost& host,
                          const Deliver& deliver, TntGravityStats& stats) {
    gameplay::ExplosionSpec spec;
    spec.centre      = blast.centre;
    spec.power       = blast.power;
    spec.interaction = blast.interaction;

    // Phase one: the rays. Nothing is written yet.
    gameplay::collect_detonation(level, explosions_, spec, random_, detonation_);

    // The entities, while every wall they might hide behind still stands.
    const AABB reach = explosions_.entity_search_box(spec);
    for (const entity::EntityHandle handle : world.handles()) {
        entity::EntityState* state = world.mutable_state(handle);
        if (state == nullptr || state->removed || state->network_id == blast.source) {
            continue;
        }
        const AABB box = gameplay::entity_box(*state);
        if (!box.intersects(reach)) {
            continue;
        }
        const gameplay::ExplosionHit hit = explosions_.hit_entity(
            level, spec, box, state->position,
            state->position.y + static_cast<f64>(state->eye_height));
        if (!hit.touched) {
            continue;
        }
        state->velocity.x += hit.impulse.x;
        state->velocity.y += hit.impulse.y;
        state->velocity.z += hit.impulse.z;
        deliver(net::clientbound::kEntityVelocity,
                net::encode_entity_velocity(state->network_id, state->velocity.x,
                                            state->velocity.y, state->velocity.z));

        // A primed TNT and a falling block are pushed and never hurt.
        if (owns(state->type) || state->max_health <= 0.0F || mob_combat_ == nullptr) {
            continue;
        }
        const MobHurt hurt = mob_combat_->hurt(*state, hit.damage, damage_constants_);
        if (!hurt.applied) {
            continue;
        }
        deliver(net::clientbound::kDamageEvent,
                net::encode_damage_event(state->network_id,
                                         damage_type_id(gameplay::DamageKind::Explosion),
                                         std::nullopt, std::nullopt));
        net::MetadataWriter fields;
        fields.float_value(net::metadata::kHealth, state->health);
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state->network_id, fields.take()));
        if (hurt.killed) {
            deliver(net::clientbound::kEntityEvent, net::encode_entity_event(state->network_id, 3));
            std::vector<gameplay::Drop> drops;
            (void)mob_combat_->loot(*state, false, 0, loot_random_, drops);
            for (const gameplay::Drop& drop : drops) {
                host.drop_item(Vec3d{state->position.x,
                                     state->position.y + static_cast<f64>(state->height) * 0.5,
                                     state->position.z},
                               net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)),
                                              {}});
            }
            state->removed = true;
            mob_combat_->forget(state->network_id);
        }
    }

    // The players: each is told about the explosion with its own knockback,
    // and hurt when it can be.
    net::Explosion packet;
    packet.x      = spec.centre.x;
    packet.y      = spec.centre.y;
    packet.z      = spec.centre.z;
    packet.radius = spec.power;
    packet.records.reserve(detonation_.blocks.size() + detonation_.air.size());
    for (const BlockPos pos : detonation_.blocks) {
        (void)packet.add_record(pos);
    }
    for (const BlockPos pos : detonation_.air) {
        (void)packet.add_record(pos);
    }
    stats.records_sent += packet.records.size();
    if (host.each_player) {
        host.each_player([&](BlastPlayer& player) {
            const Vec3d offset = player.feet - spec.centre;
            if (offset.x * offset.x + offset.y * offset.y + offset.z * offset.z >=
                kExplosionAudienceSq) {
                return;
            }
            const gameplay::ExplosionHit hit =
                explosions_.hit_entity(level, spec, gameplay::player_box(player.feet),
                                       player.feet, player.feet.y + kPlayerEye);
            packet.knockback_x = hit.touched ? static_cast<f32>(hit.impulse.x) : 0.0F;
            packet.knockback_y = hit.touched ? static_cast<f32>(hit.impulse.y) : 0.0F;
            packet.knockback_z = hit.touched ? static_cast<f32>(hit.impulse.z) : 0.0F;
            if (player.send) {
                player.send(net::clientbound::kExplosion, net::encode_explosion(packet));
            }
            if (hit.touched && !player.creative && player.hurt) {
                player.hurt(hit.damage);
            }
        });
    }

    // The loot lying on the floor goes too.
    if (host.sweep_items) {
        host.sweep_items([&](Vec3d at) {
            const AABB box = AABB::from_entity(at, 0.25, 0.25);
            if (!box.intersects(reach)) {
                return false;
            }
            const gameplay::ExplosionHit hit =
                explosions_.hit_entity(level, spec, box, at, at.y + item_eye_);
            return hit.touched && hit.damage >= kItemHealth;
        });
    }

    // Phase two: the drops, the chained TNT, and the empty cells — written
    // through the level, so the fluids and wires beside the crater hear it.
    gameplay::destroy_detonation(level, explosions_, loot_, spec, tnt_block_, random_,
                                 loot_random_, detonation_);
    for (const gameplay::Detonation::Dropped& dropped : detonation_.drops) {
        host.drop_item(block_centre(dropped.pos),
                       net::ItemStack{dropped.drop.item,
                                      static_cast<i8>(std::min(dropped.drop.count, 64)), {}});
    }
    for (const gameplay::Detonation::Primed& primed : detonation_.primed) {
        spawn_primed(world, primed.pos, primed.fuse, deliver);
    }

    ++stats.blasts;
    stats.blocks_destroyed += detonation_.blocks.size();
    stats.primed += detonation_.primed.size();
    OV_LOG_DEBUG("explosion at ({:.2f}, {:.2f}, {:.2f}) power {}: {} blocks, {} air cells, "
                 "{} drops, {} TNT lit",
                 spec.centre.x, spec.centre.y, spec.centre.z, spec.power,
                 detonation_.blocks.size(), detonation_.air.size(), detonation_.drops.size(),
                 detonation_.primed.size());
}

void TntGravity::drop_block_item(registry::BlockStateId state, BlockPos cell,
                                 const TntGravityHost& host) const {
    if (item_registry_valid_ == 0 || !host.drop_item) {
        return;
    }
    const std::string_view name = falling_.item_of(state);
    const auto             item = registries_->protocol_id(item_registry_, name);
    if (!item) {
        OV_LOG_DEBUG("falling block {} has no item to drop", name);
        return;
    }
    host.drop_item(block_centre(cell), net::ItemStack{*item, 1, {}});
}

// ── persistence ─────────────────────────────────────────────────────────────

std::optional<entity::EntityHandle> TntGravity::adopt_saved(entity::EntityWorld& world,
                                                            const nbt::Tag&      compound) {
    const nbt::Tag* id = compound.find("id");
    if (id == nullptr) {
        return std::nullopt;
    }
    const bool tnt = id->as_string() == "minecraft:tnt";
    if ((!tnt && id->as_string() != "minecraft:falling_block") || (tnt && tnt_type_ < 0) ||
        (!tnt && falling_type_ < 0)) {
        return std::nullopt;
    }
    std::optional<registry::BlockStateId> block;
    if (!tnt) {
        block = block_state_from(*blocks_, compound.find("BlockState"));
        if (!block || *block == registry::kAirState) {
            // Vanilla discards a falling block of air; an unknown block is
            // refused and carried, never turned into sand.
            OV_LOG_WARN("entities: a falling block of a state this server does not know");
            return std::nullopt;
        }
    }
    const Vec3d at = list_vec3(compound, "Pos");
    const auto  handle =
        world.spawn(tnt ? tnt_type_ : falling_type_, at,
                    uuid_from(compound.find("UUID")).value_or(net::Uuid{}));
    if (!handle) {
        OV_LOG_WARN("entities: cannot spawn a {} read from disk: {}", id->as_string(),
                    entity::to_string(handle.error()));
        return std::nullopt;
    }
    entity::EntityState* state = world.mutable_state(*handle);
    if (state->uuid == net::Uuid{}) {
        state->uuid = uuid_for(state->network_id);
    }
    state->velocity           = list_vec3(compound, "Motion");
    state->on_ground          = get_bool(compound, "OnGround", false);
    state->broadcast_position = state->position;
    state->broadcast_valid    = true;
    if (tnt) {
        // Vanilla's default when the key is missing is 80.
        const i32 fuse = static_cast<i32>(get_i64(compound, "Fuse", net::kDefaultTntFuse));
        world.set_logic(*handle, std::make_unique<gameplay::PrimedTntLogic>(fuse, blast_events_));
    } else {
        const BlockPos start{static_cast<i32>(std::floor(at.x)), static_cast<i32>(std::floor(at.y)),
                             static_cast<i32>(std::floor(at.z))};
        auto logic = std::make_unique<gameplay::FallingBlockLogic>(falling_, *block, start,
                                                                   falling_events_);
        logic->set_time(static_cast<i32>(get_i64(compound, "Time", 0)));
        world.set_logic(*handle, std::move(logic));
    }
    saved_[state->network_id] = compound;
    return *handle;
}

std::optional<nbt::Tag> TntGravity::save_entity(entity::EntityWorld& world,
                                                entity::EntityHandle handle) const {
    const entity::EntityState* state = world.state(handle);
    if (state == nullptr || state->removed) {
        return std::nullopt;
    }
    const entity::IEntityLogic* logic = world.logic(handle);
    const auto                  kept  = saved_.find(state->network_id);
    nbt::Tag out = kept != saved_.end() ? kept->second : nbt::Tag::make_compound();
    if (state->type == tnt_type_) {
        const auto* tnt = dynamic_cast<const gameplay::PrimedTntLogic*>(logic);
        if (tnt == nullptr) {
            return std::nullopt;
        }
        put_entity_base(out, "minecraft:tnt", state->position, state->velocity, state->yaw,
                        state->pitch, state->uuid, state->on_ground, i16{-1});
        (void)out.put("Fuse", nbt::Tag{static_cast<i16>(std::clamp(tnt->fuse(), -32768, 32767))});
        return out;
    }
    const auto* falling = dynamic_cast<const gameplay::FallingBlockLogic*>(logic);
    if (falling == nullptr) {
        return std::nullopt;
    }
    put_entity_base(out, "minecraft:falling_block", state->position, state->velocity, state->yaw,
                    state->pitch, state->uuid, state->on_ground, i16{0});
    (void)out.put("BlockState", block_state_tag(*blocks_, falling->state()));
    (void)out.put("Time", nbt::Tag{falling->time()});
    // What vanilla writes for a block that fell by itself (measured).
    default_to(out, "DropItem", nbt::Tag::make_bool(true));
    default_to(out, "HurtEntities", nbt::Tag::make_bool(false));
    default_to(out, "FallHurtMax", nbt::Tag{i32{40}});
    default_to(out, "FallHurtAmount", nbt::Tag{0.0F});
    default_to(out, "CancelDrop", nbt::Tag::make_bool(false));
    return out;
}

void TntGravity::release(entity::EntityWorld& world, entity::EntityHandle handle) {
    if (const entity::EntityState* state = world.state(handle)) {
        saved_.erase(state->network_id);
    }
}

}  // namespace ov::server
