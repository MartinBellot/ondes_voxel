#define OV_LOG_CATEGORY "server"

#include "end_fight.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/experience.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/raycast.hpp"
#include "ov/protocol/blast.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/survival.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/worldgen/end.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace ov::server {

namespace {

using gameplay::DragonPart;
using gameplay::DragonPhase;

/// Boss Bar actions; pink, a plain bar, the boss music (0x02) and fog (0x04).
constexpr i32 kBarAdd          = 0;
constexpr i32 kBarRemove       = 1;
constexpr i32 kBarUpdateHealth = 2;
constexpr i32 kBarPink         = 0;
constexpr i32 kBarProgress     = 0;
constexpr u8  kBarFlags        = 0x02 | 0x04;
constexpr std::string_view kBarTitle = R"({"translate":"entity.minecraft.ender_dragon"})";

/// Metadata indices, each measured on the real server (docs/provenance/dragon.md):
/// the pose (6, 7 = dying), the dragon's health (9) and phase (16); a
/// crystal's beam target (8) and base (9); a cloud's radius (8), colour (9),
/// waiting flag (10) and particle (11).
constexpr u8  kMetadataPose   = 6;
constexpr i32 kPoseDying      = 7;
constexpr u8  kMetadataHealth = 9;
constexpr u8  kMetadataPhase  = 16;
constexpr u8  kCrystalBeam    = 8;
constexpr u8  kCrystalBottom  = 9;
constexpr u8  kCloudRadius    = 8;
constexpr u8  kCloudColor     = 9;
constexpr u8  kCloudWaiting   = 10;
constexpr u8  kCloudParticle  = 11;
/// The colour the real server sends for both of the dragon's clouds.
constexpr i32 kCloudColorMeasured = 11101546;
/// A cloud's radius when the metadata carries none.
constexpr f32 kCloudDefaultRadius = 3.0F;

/// World events: the dragon's death (global), a gateway opening, the dragon
/// shooting, a dragon fireball bursting (data 1) — all four measured.
constexpr i32 kEventDragonDeath    = 1028;
constexpr i32 kEventGatewaySpawn   = 3000;
constexpr i32 kEventDragonShoot    = 1017;
constexpr i32 kEventFireballImpact = 2006;
/// Entity Event 3: the death animation, sent at the lethal hit (measured).
constexpr i8 kEntityEventDeath = 3;

/// An explosion's packet reaches players within 64 blocks (measured: a probe
/// a hundred blocks away heard none of ten crystals).
constexpr f64 kExplosionHearing = 64.0;

constexpr f64 kPlayerWidth  = 0.6;
constexpr f64 kPlayerHeight = 1.8;
constexpr f64 kPlayerEye    = 1.62;

/// The wings' shove, horizontal and up; not measured (the wiki: "thrown into
/// the air, sometimes to fatal heights or off the island").
constexpr f64 kWingPush   = 4.0;
constexpr f64 kWingLift   = 0.2;
constexpr f32 kWingDamage = 5.0F;
constexpr f32 kHeadDamage = 10.0F;

/// The respawn's stages, in ticks from the fourth crystal, measured
/// (docs/provenance/dragon.md § 7): the beam moves to pillar k at 103 + 40 k,
/// the pillar's top blows (power 5) 38 ticks later, the beams go back to
/// (0, 128, 0) at 504, the dragon comes at 604.
constexpr i32 kRespawnPillarsStart = 103;
constexpr i32 kRespawnPillarStep   = 40;
constexpr i32 kRespawnPillarBlast  = 38;
constexpr i32 kRespawnSummon       = 504;
constexpr f32 kRespawnPillarPower  = 5.0F;

[[nodiscard]] net::Uuid uuid_from(math::LegacyRandomSource& random) {
    const auto most  = static_cast<u64>(random.next_long());
    const auto least = static_cast<u64>(random.next_long());
    return net::Uuid{(most & ~0xF000ULL) | 0x4000ULL,
                     (least & ~(0xC000000000000000ULL)) | 0x8000000000000000ULL};
}

[[nodiscard]] std::vector<u8> bar_packet(const net::Uuid& uuid, i32 action) {
    io::ByteWriter writer;
    writer.write_u64(uuid.most_significant);
    writer.write_u64(uuid.least_significant);
    net::write_varint(writer, action);
    return writer.take();
}

[[nodiscard]] nbt::Tag::IntArray uuid_ints(const net::Uuid& uuid) {
    return {static_cast<i32>(static_cast<u32>(uuid.most_significant >> 32)),
            static_cast<i32>(static_cast<u32>(uuid.most_significant)),
            static_cast<i32>(static_cast<u32>(uuid.least_significant >> 32)),
            static_cast<i32>(static_cast<u32>(uuid.least_significant))};
}

[[nodiscard]] net::Uuid uuid_of(const nbt::Tag::IntArray& ints) {
    const auto word = [&](usize i) { return static_cast<u64>(static_cast<u32>(ints[i])); };
    return net::Uuid{(word(0) << 32) | word(1), (word(2) << 32) | word(3)};
}

[[nodiscard]] net::WirePosition wire(Vec3d at) {
    return net::WirePosition{static_cast<i32>(std::floor(at.x)), static_cast<i32>(std::floor(at.y)),
                             static_cast<i32>(std::floor(at.z))};
}

[[nodiscard]] BlockPos block_at(Vec3d at) {
    return BlockPos{static_cast<i32>(std::floor(at.x)), static_cast<i32>(std::floor(at.y)),
                    static_cast<i32>(std::floor(at.z))};
}

[[nodiscard]] AABB player_box(Vec3d feet) {
    return AABB::from_entity(feet, kPlayerWidth, kPlayerHeight);
}

/// A dragon fireball's cloud, measured: radius 3 growing to 7 over its 600
/// ticks (+0.00667 a tick), 20 ticks waiting, gone at 620; Harming II.
[[nodiscard]] gameplay::Cloud fireball_cloud() {
    gameplay::Cloud cloud;
    cloud.radius              = 3.0F;
    cloud.radius_on_use       = 0.0F;
    cloud.radius_per_tick     = (7.0F - 3.0F) / 600.0F;
    cloud.duration            = 600;
    cloud.duration_on_use     = 0;
    cloud.wait_time           = 20;
    cloud.reapplication_delay = 20;
    return cloud;
}

/// The breath, measured: radius 5 that does not grow, 20 ticks waiting,
/// removed when the flaming ends (190 ticks after it appeared); Harming I.
[[nodiscard]] gameplay::Cloud breath_cloud() {
    gameplay::Cloud cloud = fireball_cloud();
    cloud.radius          = 5.0F;
    cloud.radius_per_tick = 0.0F;
    cloud.duration        = 200;
    return cloud;
}

/// The first free block of a column: one above its highest non-air block.
[[nodiscard]] i32 column_top(const world::LevelView& level, i32 x, i32 z) {
    const world::WorldShape shape = level.shape();
    for (i32 y = shape.max_y(); y >= shape.min_y; --y) {
        if (level.block_at(BlockPos{x, y, z}) != registry::kAirState) {
            return y + 1;
        }
    }
    return shape.min_y;
}

}  // namespace

// ── Construction and save ───────────────────────────────────────────────────

EndFight::EndFight(const gameplay::EndPortalRules& rules, const registry::BlockRegistry& blocks,
                   i64 seed, EndFightTypes types, std::span<const registry::BlockId> immune,
                   std::span<const registry::BlockId> transparent)
    : rules_(&rules),
      blocks_(&blocks),
      seed_(seed),
      types_(types),
      explosions_(blocks),
      random_(seed ^ 0x454E44L) {
    immune_.assign(blocks.block_count(), false);
    transparent_.assign(blocks.block_count(), false);
    for (const registry::BlockId block : immune) {
        if (block.value() < immune_.size()) {
            immune_[block.value()] = true;
        }
    }
    for (const registry::BlockId block : transparent) {
        if (block.value() < transparent_.size()) {
            transparent_[block.value()] = true;
        }
    }
    const auto state_of = [&](std::string_view name) {
        const auto block = blocks.find_block(name);
        return block ? blocks.default_state(*block) : registry::kAirState;
    };
    fire_      = state_of("minecraft:fire");
    obsidian_  = state_of("minecraft:obsidian");
    bedrock_   = state_of("minecraft:bedrock");
    iron_bars_ = state_of("minecraft:iron_bars");
    const auto order = gameplay::end_gateway_indices(seed);
    gateways_.assign(order.begin(), order.end());
    crystals_.reserve(32);
    fireballs_.reserve(16);
    clouds_.reserve(16);
    orbs_.reserve(256);
    blasts_.reserve(16);
    players_.reserve(16);
    targets_.reserve(16);
    cells_.reserve(4096);
    air_.reserve(4096);
    gone_.reserve(16);
}

f32 EndFight::dragon_health() const noexcept {
    return dragon_ ? dragon_->health() : 0.0F;
}

Vec3d EndFight::dragon_position() const noexcept {
    return dragon_ ? dragon_->position() : Vec3d{0.0, 128.0, 0.0};
}

usize EndFight::crystals_alive() const noexcept {
    return static_cast<usize>(
        std::ranges::count_if(crystals_, [](const Crystal& c) { return c.alive; }));
}

bool EndFight::within_range(Vec3d position) noexcept {
    const f64 dx = position.x;
    const f64 dy = position.y - 128.0;
    const f64 dz = position.z;
    return dx * dx + dy * dy + dz * dz <= 192.0 * 192.0;
}

bool EndFight::immune(registry::BlockStateId state) const noexcept {
    const usize block = blocks_->block_of(state).value();
    return block < immune_.size() && immune_[block];
}

bool EndFight::transparent(registry::BlockStateId state) const noexcept {
    const usize block = blocks_->block_of(state).value();
    return block < transparent_.size() && transparent_[block];
}

net::Uuid EndFight::next_uuid() {
    return uuid_from(random_);
}

void EndFight::load(const nbt::Tag& fight) {
    const auto flag = [&](std::string_view key, bool& into) {
        if (const nbt::Tag* value = fight.find(key)) {
            into = value->as_bool();
        }
    };
    flag("DragonKilled", killed_);
    flag("PreviouslyKilled", previously_killed_);
    flag("NeedsStateScanning", needs_scanning_);
    if (const nbt::Tag* where = fight.find("ExitPortalLocation")) {
        if (const auto* ints = where->get_if<nbt::Tag::IntArray>(); ints && ints->size() == 3) {
            portal_ = BlockPos{(*ints)[0], (*ints)[1], (*ints)[2]};
        } else if (where->find("X") != nullptr) {
            // The compound form of older saves.
            portal_ = BlockPos{static_cast<i32>(where->find("X")->as_i64()),
                               static_cast<i32>(where->find("Y") ? where->find("Y")->as_i64() : 0),
                               static_cast<i32>(where->find("Z") ? where->find("Z")->as_i64() : 0)};
        }
    }
    if (const nbt::Tag* list = fight.find("Gateways")) {
        if (const auto* ints = list->get_if<nbt::Tag::IntArray>()) {
            gateways_.clear();
            for (const i32 index : *ints) {
                if (index >= 0 && index < 20) {
                    gateways_.push_back(index);
                }
            }
        }
    }
    if (const nbt::Tag* uuid = fight.find("Dragon")) {
        if (const auto* ints = uuid->get_if<nbt::Tag::IntArray>(); ints && ints->size() == 4) {
            dragon_uuid_ = uuid_of(*ints);
        }
    }
    // A fight whose dragon is dead has its arena: nothing to build on arrival.
    started_ = killed_ && portal_.has_value();
    OV_LOG_INFO("end: DragonFight read — killed {}, previously killed {}, {} gateways left{}",
                killed_, previously_killed_, gateways_.size(),
                portal_ ? " , exit portal known" : "");
}

nbt::Tag EndFight::save() const {
    // The real server's keys and types (measured after a death): the portal,
    // the gateways and the dragon's UUID as int arrays; the UUID kept after it
    // died.
    nbt::Tag fight = nbt::Tag::make_compound();
    fight.put("NeedsStateScanning", nbt::Tag::make_bool(!started_ && needs_scanning_));
    if (portal_) {
        fight.put("ExitPortalLocation", nbt::Tag{nbt::Tag::IntArray{portal_->x, portal_->y,
                                                                     portal_->z}});
    }
    fight.put("Gateways", nbt::Tag{nbt::Tag::IntArray(gateways_.begin(), gateways_.end())});
    fight.put("DragonKilled", nbt::Tag::make_bool(killed_));
    fight.put("PreviouslyKilled", nbt::Tag::make_bool(previously_killed_));
    if (dragon_uuid_) {
        fight.put("Dragon", nbt::Tag{uuid_ints(*dragon_uuid_)});
    }
    return fight;
}

// ── The start, and what a newcomer is sent ──────────────────────────────────

void EndFight::start(world::LevelWriter& level, i32 origin_top, const EndFightHost& host) {
    if (started_) {
        return;
    }
    started_        = true;
    needs_scanning_ = false;
    portal_         = rules_->exit_portal_origin(level, origin_top);
    rules_->build_exit_portal(level, *portal_, false);
    if (killed_) {
        return;
    }
    spawn_dragon(host);
    const auto spikes = worldgen::end_spikes(seed_);
    for (const worldgen::EndSpike& spike : spikes) {
        Crystal crystal;
        crystal.entity_id   = host.reserve_entity_ids(1);
        crystal.uuid        = next_uuid();
        crystal.position    = Vec3d{static_cast<f64>(spike.centre_x) + 0.5,
                                 static_cast<f64>(spike.height) + 1.0,
                                 static_cast<f64>(spike.centre_z) + 0.5};
        crystals_.push_back(crystal);
    }
    OV_LOG_INFO("end: the dragon fight starts — exit portal at ({}, {}, {}), the dragon at "
                "(0, 128, 0), {} crystals",
                portal_->x, portal_->y, portal_->z, crystals_.size());
}

void EndFight::spawn_dragon(const EndFightHost& host) {
    // Nine ids: the dragon and its eight parts, which the client numbers
    // after it — a hit on the head arrives with the dragon's id plus one.
    dragon_id_ = host.reserve_entity_ids(9);
    if (!dragon_uuid_) {
        dragon_uuid_ = next_uuid();
    }
    bar_uuid_       = next_uuid();
    const f32 yaw   = random_.next_float() * 360.0F;
    dragon_.emplace(Vec3d{0.0, 128.0, 0.0}, yaw, kDragonMaxHealth, previously_killed_);
    dragon_->set_phase(DragonPhase::HoldingPattern);
    world_.sees  = sees_;
    sent_health_ = kDragonMaxHealth;
    nearest_     = -1;
}

std::vector<u8> EndFight::boss_bar_add() const {
    io::ByteWriter writer;
    writer.write_u64(bar_uuid_.most_significant);
    writer.write_u64(bar_uuid_.least_significant);
    net::write_varint(writer, kBarAdd);
    net::write_varint(writer, static_cast<i32>(kBarTitle.size()));
    writer.write_bytes(kBarTitle);
    writer.write_f32(dragon_health() / kDragonMaxHealth);
    net::write_varint(writer, kBarPink);
    net::write_varint(writer, kBarProgress);
    writer.write_u8(kBarFlags);
    return writer.take();
}

void EndFight::send_crystal(const Crystal& crystal,
                            const std::function<void(i32, std::span<const u8>)>& send) const {
    net::SpawnEntity spawn;
    spawn.entity_id = crystal.entity_id;
    spawn.uuid      = crystal.uuid;
    spawn.type      = types_.crystal;
    spawn.x         = crystal.position.x;
    spawn.y         = crystal.position.y;
    spawn.z         = crystal.position.z;
    send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    net::MetadataWriter fields;
    if (!crystal.show_bottom) {
        fields.boolean_value(kCrystalBottom, false);
    }
    if (crystal.beam) {
        fields.optional_block_pos_value(
            kCrystalBeam, net::WirePosition{crystal.beam->x, crystal.beam->y, crystal.beam->z});
    }
    if (!fields.empty()) {
        send(net::clientbound::kEntityMetadata,
             net::encode_entity_metadata(crystal.entity_id, fields.take()));
    }
}

void EndFight::send_cloud(const CloudEntity& cloud,
                          const std::function<void(i32, std::span<const u8>)>& send) const {
    net::SpawnEntity spawn;
    spawn.entity_id = cloud.entity_id;
    spawn.uuid      = cloud.uuid;
    spawn.type      = types_.cloud;
    spawn.x         = cloud.position.x;
    spawn.y         = cloud.position.y;
    spawn.z         = cloud.position.z;
    send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    net::MetadataWriter fields;
    if (cloud.cloud.radius != kCloudDefaultRadius) {
        fields.float_value(kCloudRadius, cloud.cloud.radius);
    }
    fields.varint_value(kCloudColor, kCloudColorMeasured);
    fields.boolean_value(kCloudWaiting, gameplay::cloud_waiting(cloud.cloud));
    fields.particle_value(kCloudParticle, types_.breath_particle);
    send(net::clientbound::kEntityMetadata,
         net::encode_entity_metadata(cloud.entity_id, fields.take()));
}

void EndFight::show_to(const std::function<void(i32, std::span<const u8>)>& send) const {
    if (!started_) {
        return;
    }
    if (dragon_) {
        net::SpawnEntity spawn;
        spawn.entity_id = dragon_id_;
        spawn.uuid      = *dragon_uuid_;
        spawn.type      = types_.dragon;
        spawn.x         = dragon_->position().x;
        spawn.y         = dragon_->position().y;
        spawn.z         = dragon_->position().z;
        spawn.yaw       = dragon_->wire_yaw();
        spawn.head_yaw  = dragon_->wire_yaw();
        send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
        net::MetadataWriter fields;
        fields.float_value(kMetadataHealth, dragon_->health());
        fields.varint_value(kMetadataPhase, static_cast<i32>(dragon_->phase()));
        if (dragon_->phase() == DragonPhase::Dying || dragon_->health() <= 0.0F) {
            fields.pose_value(kMetadataPose, kPoseDying);
        }
        send(net::clientbound::kEntityMetadata,
             net::encode_entity_metadata(dragon_id_, fields.take()));
        send(kBossBarPacket, boss_bar_add());
    }
    for (const Crystal& crystal : crystals_) {
        if (crystal.alive) {
            send_crystal(crystal, send);
        }
    }
    for (const Fireball& fireball : fireballs_) {
        net::SpawnEntity spawn;
        spawn.entity_id = fireball.entity_id;
        spawn.uuid      = fireball.uuid;
        spawn.type      = types_.fireball;
        spawn.x         = fireball.position.x;
        spawn.y         = fireball.position.y;
        spawn.z         = fireball.position.z;
        send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    }
    for (const CloudEntity& cloud : clouds_) {
        send_cloud(cloud, send);
    }
    for (const Orb& orb : orbs_) {
        send(net::clientbound::kSpawnExperienceOrb,
             net::encode_spawn_experience_orb(orb.entity_id, orb.position.x, orb.position.y,
                                              orb.position.z, static_cast<i16>(orb.value)));
    }
}

void EndFight::hide_from(const std::function<void(i32, std::span<const u8>)>& send) const {
    if (started_ && dragon_) {
        send(kBossBarPacket, bar_packet(bar_uuid_, kBarRemove));
    }
}

// ── Packets about the dragon ────────────────────────────────────────────────

void EndFight::set_health_packets(const EndFightHost& host) {
    if (!dragon_) {
        return;
    }
    const f32 health = std::max(dragon_->health(), 0.0F);
    if (health == sent_health_) {
        return;
    }
    sent_health_ = health;
    net::MetadataWriter fields;
    fields.float_value(kMetadataHealth, health);
    host.broadcast(net::clientbound::kEntityMetadata,
                   net::encode_entity_metadata(dragon_id_, fields.take()));
    auto           bar = bar_packet(bar_uuid_, kBarUpdateHealth);
    io::ByteWriter tail;
    tail.write_f32(health / kDragonMaxHealth);
    bar.insert(bar.end(), tail.data().begin(), tail.data().end());
    host.broadcast(kBossBarPacket, bar);
}

void EndFight::send_phase(const EndFightHost& host) {
    if (!dragon_) {
        return;
    }
    net::MetadataWriter fields;
    fields.varint_value(kMetadataPhase, static_cast<i32>(dragon_->phase()));
    host.broadcast(net::clientbound::kEntityMetadata,
                   net::encode_entity_metadata(dragon_id_, fields.take()));
}

void EndFight::death_packets(const EndFightHost& host, DragonPhase before, f32 before_health) {
    if (!dragon_) {
        return;
    }
    const bool dying  = dragon_->phase() == DragonPhase::Dying && before != DragonPhase::Dying;
    const bool killed = before_health > 0.0F && dragon_->health() <= 0.0F;
    if (!dying && !killed) {
        return;
    }
    // Measured at the lethal hit: Entity Event 3, then the pose, the health
    // and the phase in one metadata packet.
    host.broadcast(net::clientbound::kEntityEvent,
                   net::encode_entity_event(dragon_id_, kEntityEventDeath));
    net::MetadataWriter fields;
    fields.pose_value(kMetadataPose, kPoseDying);
    fields.float_value(kMetadataHealth, std::max(dragon_->health(), 0.0F));
    fields.varint_value(kMetadataPhase, static_cast<i32>(dragon_->phase()));
    host.broadcast(net::clientbound::kEntityMetadata,
                   net::encode_entity_metadata(dragon_id_, fields.take()));
    sent_health_ = std::max(dragon_->health(), 0.0F);
    OV_LOG_INFO("end: the dragon is dying");
}

// ── Hits, kills and the command engine ──────────────────────────────────────

std::optional<i32> EndFight::strafe_target(std::optional<i32> attacker, bool mortal,
                                           Vec3d at) const {
    // The destroyer, if it can be targeted; nobody if it was a creative
    // player; otherwise the targetable player nearest the crystal.
    if (attacker) {
        return mortal ? attacker : std::nullopt;
    }
    std::optional<i32> nearest;
    f64                best = std::numeric_limits<f64>::infinity();
    for (const EndFightPlayer& p : players_) {
        if (!p.mortal || !p.alive) {
            continue;
        }
        const f64 d = (p.feet - at).length_squared();
        if (d < best) {
            best    = d;
            nearest = p.id;
        }
    }
    return nearest;
}

bool EndFight::hurt(i32 entity_id, f32 damage, const EndFightHost& host,
                    std::optional<i32> attacker, bool mortal) {
    if (!started_) {
        return false;
    }
    for (usize i = 0; i < crystals_.size(); ++i) {
        Crystal& crystal = crystals_[i];
        if (crystal.entity_id != entity_id) {
            continue;
        }
        // During a respawn the spikes' crystals cannot be destroyed (wiki).
        if (crystal.alive && !(respawn_ticks_ >= 0 && !crystal.placed)) {
            destroy_crystal(i, true, strafe_target(attacker, mortal, crystal.position),
                            attacker, host);
        }
        return true;
    }
    for (const Fireball& fireball : fireballs_) {
        if (fireball.entity_id == entity_id) {
            return true;  // a dragon fireball cannot be deflected
        }
    }
    if (!dragon_ || entity_id < dragon_id_ || entity_id > dragon_id_ + 8) {
        return false;
    }
    const DragonPart part = entity_id == dragon_id_
                                ? DragonPart::Body
                                : static_cast<DragonPart>(entity_id - dragon_id_ - 1);
    const DragonPhase before        = dragon_->phase();
    const f32         before_health = dragon_->health();
    if (dragon_->hurt(part, damage, gameplay::DragonHurtSource::Player) > 0.0F) {
        death_packets(host, before, before_health);
        set_health_packets(host);
    }
    (void)damage;
    return true;
}

bool EndFight::owns(i32 entity_id) const noexcept {
    if (dragon_ && entity_id >= dragon_id_ && entity_id <= dragon_id_ + 8) {
        return true;
    }
    return std::ranges::any_of(crystals_,
                               [&](const Crystal& c) { return c.alive && c.entity_id == entity_id; }) ||
           std::ranges::any_of(fireballs_,
                               [&](const Fireball& f) { return f.entity_id == entity_id; }) ||
           std::ranges::any_of(clouds_,
                               [&](const CloudEntity& c) { return c.entity_id == entity_id; }) ||
           std::ranges::any_of(orbs_, [&](const Orb& o) { return o.entity_id == entity_id; });
}

bool EndFight::kill(i32 entity_id, const EndFightHost& host) {
    if (!started_) {
        return false;
    }
    if (dragon_ && entity_id >= dragon_id_ && entity_id <= dragon_id_ + 8) {
        // No death animation, no experience: measured. The portal is built by
        // the next tick, the End's only writer.
        kill_pending_ = true;
        return true;
    }
    for (usize i = 0; i < crystals_.size(); ++i) {
        if (crystals_[i].alive && crystals_[i].entity_id == entity_id) {
            destroy_crystal(i, false, strafe_target(std::nullopt, true, crystals_[i].position),
                            std::nullopt, host);
            return true;
        }
    }
    const auto remove = [&](auto& list) {
        for (usize i = 0; i < list.size(); ++i) {
            if (list[i].entity_id == entity_id) {
                host.broadcast(net::clientbound::kRemoveEntities,
                               net::encode_remove_entity(entity_id));
                list.erase(list.begin() + static_cast<isize>(i));
                return true;
            }
        }
        return false;
    };
    return remove(fireballs_) || remove(clouds_) || remove(orbs_);
}

void EndFight::entities(std::vector<EndFightEntity>& out) const {
    if (dragon_) {
        out.push_back(EndFightEntity{dragon_id_, "minecraft:ender_dragon", *dragon_uuid_,
                                     dragon_->position(), 16.0F, 8.0F});
    }
    for (const Crystal& c : crystals_) {
        if (c.alive) {
            out.push_back(EndFightEntity{c.entity_id, "minecraft:end_crystal", c.uuid, c.position,
                                         2.0F, 2.0F});
        }
    }
    for (const Fireball& f : fireballs_) {
        out.push_back(EndFightEntity{f.entity_id, "minecraft:dragon_fireball", f.uuid, f.position,
                                     1.0F, 1.0F});
    }
    for (const CloudEntity& c : clouds_) {
        out.push_back(EndFightEntity{c.entity_id, "minecraft:area_effect_cloud", c.uuid,
                                     c.position, c.cloud.radius * 2.0F, 0.5F});
    }
    for (const Orb& o : orbs_) {
        out.push_back(EndFightEntity{o.entity_id, "minecraft:experience_orb", net::Uuid{},
                                     o.position, 0.5F, 0.5F});
    }
}

void EndFight::place_crystal(BlockPos on, const EndFightHost& host) {
    Crystal crystal;
    crystal.entity_id   = host.reserve_entity_ids(1);
    crystal.uuid        = next_uuid();
    crystal.position    = Vec3d{static_cast<f64>(on.x) + 0.5, static_cast<f64>(on.y) + 1.0,
                             static_cast<f64>(on.z) + 0.5};
    crystal.show_bottom = false;
    crystal.placed      = true;
    crystals_.push_back(crystal);
    send_crystal(crystals_.back(), host.broadcast);
    respawn_check_ = true;
}

bool EndFight::take_breath(Vec3d feet) {
    // The bottle reaches any of the dragon's clouds whose position is within
    // two blocks of the player's box; the cloud loses half a block.
    const AABB reach = player_box(feet).inflated(2.0);
    for (CloudEntity& cloud : clouds_) {
        if (cloud.cloud.radius > 0.0F && reach.contains(cloud.position)) {
            cloud.cloud.radius -= 0.5F;
            return true;
        }
    }
    return false;
}

// ── The tick ────────────────────────────────────────────────────────────────

void EndFight::tick(world::LevelWriter& level, const EndFightHost& host) {
    if (!started_) {
        return;
    }
    ++ticks_;
    players_.clear();
    if (host.players) {
        host.players(players_);
    }
    if (ticks_ % 20 == 1 || fountain_ <= 0) {
        fountain_ = column_top(level, 0, 0);
    }
    // The crystal count, as the dragon reads it: every 100 ticks while a
    // player is there and the dragon is alive (and on each destruction).
    if (dragon_ && !players_.empty() && ++count_timer_ >= 100) {
        count_timer_ = 0;
        recount_crystals();
    }
    if (kill_pending_) {
        kill_pending_ = false;
        finish_death(level, host, false);
    }
    for (usize i = 0; i < blasts_.size(); ++i) {
        const PendingBlast pending = blasts_[i];
        blast(level, pending, host);
    }
    blasts_.clear();
    tick_respawn(level, host);
    tick_dragon(level, host);
    tick_crystals(level, host);
    tick_fireballs(level, host);
    tick_clouds(host);
    tick_orbs(level, host);
}

void EndFight::recount_crystals() {
    // The crystals within the spikes' boxes, extended to the top of the world.
    const auto spikes = worldgen::end_spikes(seed_);
    i32        count  = 0;
    for (const Crystal& crystal : crystals_) {
        if (!crystal.alive) {
            continue;
        }
        for (const worldgen::EndSpike& spike : spikes) {
            const f64 r = static_cast<f64>(spike.radius);
            if (crystal.position.x >= static_cast<f64>(spike.centre_x) - r &&
                crystal.position.x <= static_cast<f64>(spike.centre_x) + r + 1.0 &&
                crystal.position.z >= static_cast<f64>(spike.centre_z) - r &&
                crystal.position.z <= static_cast<f64>(spike.centre_z) + r + 1.0) {
                ++count;
                break;
            }
        }
    }
    crystal_count_ = count;
}

void EndFight::tick_dragon(world::LevelWriter& level, const EndFightHost& host) {
    if (!dragon_) {
        return;
    }
    gameplay::Dragon& dragon = *dragon_;
    if (!dragon.graph().placed()) {
        dragon.graph().place([&](i32 x, i32 z) { return column_top(level, x, z); });
    }
    targets_.clear();
    for (const EndFightPlayer& p : players_) {
        if (p.mortal && p.alive) {
            targets_.push_back(gameplay::DragonPlayer{p.id, p.feet, kPlayerEye});
        }
    }
    world_.crystals     = crystal_count_;
    world_.players      = targets_;
    world_.fountain_top = fountain_;

    gameplay::DragonOutput out;
    dragon.tick(world_, random_, out);

    const Vec3d at = dragon.position();
    host.broadcast(net::clientbound::kEntityTeleport,
                   net::encode_entity_teleport(dragon_id_, at.x, at.y, at.z, dragon.wire_yaw(),
                                               dragon.pitch(), false));
    if (out.phase_changed) {
        send_phase(host);
    }
    if (dragon.health() <= 0.0F && dragon.death_time() == 1) {
        // The animation begins: the health falls to zero, the death is heard
        // everywhere (measured: both in the same tick).
        set_health_packets(host);
        host.broadcast(net::clientbound::kWorldEvent,
                       net::encode_world_event(kEventDragonDeath, wire(at), 0, true));
    }
    if (out.fireball) {
        host.broadcast(net::clientbound::kWorldEvent,
                       net::encode_world_event(kEventDragonShoot, wire(at), 0, false));
        spawn_fireball(out.fireball_from, out.fireball_direction, host);
    }
    if (out.breath_start) {
        // On the first non-air block under the point ahead of the head.
        Vec3d          ground = out.breath_from;
        const world::WorldShape shape = level.shape();
        i32 y = static_cast<i32>(std::floor(ground.y));
        const i32 x = static_cast<i32>(std::floor(ground.x));
        const i32 z = static_cast<i32>(std::floor(ground.z));
        while (y > shape.min_y && level.block_at(BlockPos{x, y - 1, z}) == registry::kAirState) {
            --y;
        }
        ground.y = static_cast<f64>(y);
        spawn_cloud(ground, true, host);
    }
    if (out.breath_stop) {
        for (usize i = clouds_.size(); i-- > 0;) {
            if (clouds_[i].breath) {
                host.broadcast(net::clientbound::kRemoveEntities,
                               net::encode_remove_entity(clouds_[i].entity_id));
                clouds_.erase(clouds_.begin() + static_cast<isize>(i));
            }
        }
    }
    if (out.experience > 0) {
        spawn_orbs(at, out.experience, host);
        experience_dropped_ += out.experience;
    }

    // The crystal drawing on it heals it a point every ten ticks; another is
    // looked for one tick in ten, within 32 blocks of its box.
    if (nearest_ >= 0) {
        if (!crystals_[static_cast<usize>(nearest_)].alive) {
            nearest_ = -1;
        } else if (ticks_ % 10 == 0) {
            dragon.heal(1.0F);
        }
    }
    if (random_.next_int(10) == 0) {
        f64 best = 0.0;
        nearest_ = -1;
        for (usize i = 0; i < crystals_.size(); ++i) {
            const Crystal& crystal = crystals_[i];
            if (!crystal.alive) {
                continue;
            }
            const Vec3d& p = crystal.position;
            if (std::abs(p.x - at.x) > 40.0 || std::abs(p.z - at.z) > 40.0 || p.y < at.y - 32.0 ||
                p.y > at.y + 40.0) {
                continue;
            }
            const f64 d = (p - at).length_squared();
            if (nearest_ < 0 || d < best) {
                best     = d;
                nearest_ = static_cast<i32>(i);
            }
        }
    }
    set_health_packets(host);
    if (dragon.health() > 0.0F) {
        body_against_world(level, host);
    }
    if (out.dead) {
        finish_death(level, host, true);
    }
}

void EndFight::body_against_world(world::LevelWriter& level, const EndFightHost& host) {
    gameplay::Dragon& dragon  = *dragon_;
    const bool        sitting = gameplay::dragon_sitting(dragon.phase());
    const auto        parts   = dragon.parts();
    if (!sitting) {
        // The head, the neck and the body go through what they meet: anything
        // but the immune and the transparent is removed (mobGriefing on); an
        // immune block makes it a dragon in a wall.
        bool in_wall = false;
        for (const DragonPart which : {DragonPart::Head, DragonPart::Neck, DragonPart::Body}) {
            const AABB& box = parts[static_cast<usize>(which)];
            for (i32 x = static_cast<i32>(std::floor(box.min.x));
                 x <= static_cast<i32>(std::floor(box.max.x)); ++x) {
                for (i32 y = static_cast<i32>(std::floor(box.min.y));
                     y <= static_cast<i32>(std::floor(box.max.y)); ++y) {
                    for (i32 z = static_cast<i32>(std::floor(box.min.z));
                         z <= static_cast<i32>(std::floor(box.max.z)); ++z) {
                        const BlockPos               pos{x, y, z};
                        const registry::BlockStateId state = level.block_at(pos);
                        if (state == registry::kAirState ||
                            blocks_->is_air(blocks_->block_of(state)) || transparent(state)) {
                            continue;
                        }
                        if (immune(state)) {
                            in_wall = true;
                            continue;
                        }
                        if (blocks_->holds_fluid(state) && !blocks_->blocks_motion(blocks_->block_of(state))) {
                            continue;  // water and lava stay
                        }
                        level.set_block(pos, registry::kAirState);
                    }
                }
            }
        }
        dragon.set_in_wall(in_wall);
    }
    // The wings throw, the head and neck bite — neither while perched, nor in
    // the half second after the dragon was hit (wiki). Every five ticks here.
    if (sitting || dragon.since_hurt() < 10 || ticks_ % 5 != 0) {
        return;
    }
    const Vec3d centre = parts[static_cast<usize>(DragonPart::Body)].centre();
    const auto  wing   = [&](DragonPart which) {
        const AABB& w = parts[static_cast<usize>(which)];
        return AABB{Vec3d{w.min.x - 4.0, w.min.y - 4.0, w.min.z - 4.0},
                    Vec3d{w.max.x + 4.0, w.max.y, w.max.z + 4.0}};
    };
    const AABB left  = wing(DragonPart::Wing1);
    const AABB right = wing(DragonPart::Wing2);
    const AABB head  = parts[static_cast<usize>(DragonPart::Head)].inflated(1.0);
    const AABB neck  = parts[static_cast<usize>(DragonPart::Neck)].inflated(1.0);
    for (const EndFightPlayer& p : players_) {
        if (!p.alive) {
            continue;
        }
        const AABB box = player_box(p.feet);
        if (box.intersects(left) || box.intersects(right)) {
            const f64 dx       = p.feet.x - centre.x;
            const f64 dz       = p.feet.z - centre.z;
            const f64 distance = std::max(std::sqrt(dx * dx + dz * dz), 0.1);
            if (host.send_to) {
                host.send_to(p.id, net::clientbound::kEntityVelocity,
                             net::encode_entity_velocity(p.id, dx / distance * kWingPush, kWingLift,
                                                         dz / distance * kWingPush));
            }
            if (p.mortal && host.hurt_player) {
                (void)host.hurt_player(p.id, kWingDamage, gameplay::DamageKind::MobAttack);
            }
        }
        if ((box.intersects(head) || box.intersects(neck)) && p.mortal && host.hurt_player) {
            (void)host.hurt_player(p.id, kHeadDamage, gameplay::DamageKind::MobAttack);
        }
    }
}

void EndFight::tick_crystals(world::LevelWriter& level, const EndFightHost& /*host*/) {
    // In the End a crystal keeps a fire burning in its own block (wiki) —
    // which is the fire the real server's pillars carry (docs/provenance/end.md).
    if (fire_ == registry::kAirState) {
        return;
    }
    for (const Crystal& crystal : crystals_) {
        if (!crystal.alive) {
            continue;
        }
        const BlockPos cell = block_at(crystal.position);
        if (level.is_loaded(cell) && level.block_at(cell) == registry::kAirState) {
            level.set_block(cell, fire_);
        }
    }
}

// ── Crystals and blasts ─────────────────────────────────────────────────────

void EndFight::destroy_crystal(usize index, bool explode, std::optional<i32> strafe,
                               std::optional<i32> attacker, const EndFightHost& host) {
    Crystal& crystal = crystals_[index];
    crystal.alive    = false;
    host.broadcast(net::clientbound::kRemoveEntities, net::encode_remove_entity(crystal.entity_id));
    if (explode) {
        blasts_.push_back(PendingBlast{crystal.position, attacker});
    }
    if (respawn_ticks_ >= 0 &&
        std::ranges::find(respawn_crystals_, index) != respawn_crystals_.end()) {
        // One of the four is gone: the respawn is cancelled (wiki).
        respawn_ticks_ = -1;
        OV_LOG_INFO("end: a respawn crystal was destroyed — the respawn is cancelled");
    }
    if (dragon_ && nearest_ == static_cast<i32>(index)) {
        nearest_ = -1;
        const DragonPhase before        = dragon_->phase();
        const f32         before_health = dragon_->health();
        dragon_->lose_crystal();  // 10, measured
        death_packets(host, before, before_health);
        set_health_packets(host);
    }
    recount_crystals();
    if (dragon_) {
        dragon_->crystal_destroyed(strafe);
    }
    OV_LOG_INFO("end: a crystal is destroyed{}, {} left", explode ? " and explodes" : "",
                crystals_alive());
}

void EndFight::blast(world::LevelWriter& level, const PendingBlast& pending,
                     const EndFightHost& host) {
    const gameplay::ExplosionSpec spec{pending.centre, pending.power,
                                       pending.keep_blocks ? gameplay::BlockInteraction::Keep
                                                           : gameplay::BlockInteraction::Destroy,
                                       false};
    cells_.clear();
    air_.clear();
    if (!pending.keep_blocks) {
        explosions_.collect_cells(level, spec, random_, cells_, air_);
        for (const BlockPos cell : cells_) {
            level.set_block(cell, registry::kAirState);  // no drops: named in the header
        }
    }
    const gameplay::DamageKind kind =
        pending.attacker ? gameplay::DamageKind::PlayerExplosion : gameplay::DamageKind::Explosion;
    for (const EndFightPlayer& p : players_) {
        net::Explosion packet;
        packet.x      = pending.centre.x;
        packet.y      = pending.centre.y;
        packet.z      = pending.centre.z;
        packet.radius = pending.power;
        if (p.alive) {
            const gameplay::ExplosionHit hit = explosions_.hit_entity(
                level, spec, player_box(p.feet), p.feet, p.feet.y + kPlayerEye);
            if (hit.touched) {
                packet.knockback_x = static_cast<f32>(hit.impulse.x);
                packet.knockback_y = static_cast<f32>(hit.impulse.y);
                packet.knockback_z = static_cast<f32>(hit.impulse.z);
                if (p.mortal && hit.damage > 0.0F && host.hurt_player) {
                    (void)host.hurt_player(p.id, hit.damage, kind);
                }
            }
        }
        if ((p.feet - pending.centre).length_squared() >= kExplosionHearing * kExplosionHearing ||
            !host.send_to) {
            continue;
        }
        for (const BlockPos cell : cells_) {
            (void)packet.add_record(cell);
        }
        for (const BlockPos cell : air_) {
            (void)packet.add_record(cell);
        }
        host.send_to(p.id, net::clientbound::kExplosion, net::encode_explosion(packet));
    }
    // The dragon takes explosions through its parts (the cooldown keeps it to
    // one), and a crystal a blast reaches disappears without exploding (wiki).
    if (dragon_ && dragon_->health() > 0.0F) {
        const auto parts = dragon_->parts();
        for (usize i = 0; i < parts.size(); ++i) {
            const AABB&                  box = parts[i];
            const Vec3d                  feet{box.centre().x, box.min.y, box.centre().z};
            const gameplay::ExplosionHit hit =
                explosions_.hit_entity(level, spec, box, feet, box.centre().y);
            if (hit.touched && hit.damage > 0.0F) {
                const DragonPhase before        = dragon_->phase();
                const f32         before_health = dragon_->health();
                (void)dragon_->hurt(static_cast<DragonPart>(i), hit.damage,
                                    gameplay::DragonHurtSource::Explosion);
                death_packets(host, before, before_health);
            }
        }
        set_health_packets(host);
    }
    for (usize i = 0; i < crystals_.size(); ++i) {
        Crystal& other = crystals_[i];
        if (!other.alive || (respawn_ticks_ >= 0 && !other.placed)) {
            continue;
        }
        const AABB box = AABB::from_entity(other.position, 2.0, 2.0);
        const gameplay::ExplosionHit hit =
            explosions_.hit_entity(level, spec, box, other.position, other.position.y + 1.0);
        if (hit.touched && hit.damage > 0.0F) {
            destroy_crystal(i, false, strafe_target(pending.attacker, true, other.position),
                            pending.attacker, host);
        }
    }
}

// ── Fireballs, clouds, orbs ─────────────────────────────────────────────────

void EndFight::spawn_fireball(Vec3d from, Vec3d direction, const EndFightHost& host) {
    // Measured: it starts at rest and gathers speed at 0.1 a tick against a
    // drag of 0.95 — 53.4 blocks in 46 ticks by that rule, 53.6 measured.
    Fireball fireball;
    fireball.entity_id = host.reserve_entity_ids(1);
    fireball.uuid      = next_uuid();
    fireball.position  = from;
    fireball.power     = direction * 0.1;
    net::SpawnEntity spawn;
    spawn.entity_id = fireball.entity_id;
    spawn.uuid      = fireball.uuid;
    spawn.type      = types_.fireball;
    spawn.x         = from.x;
    spawn.y         = from.y;
    spawn.z         = from.z;
    host.broadcast(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    fireballs_.push_back(fireball);
}

void EndFight::tick_fireballs(world::LevelWriter& level, const EndFightHost& host) {
    for (usize i = 0; i < fireballs_.size();) {
        Fireball& fireball = fireballs_[i];
        fireball.velocity  = (fireball.velocity + fireball.power) * 0.95;
        const Vec3d next   = fireball.position + fireball.velocity;
        ++fireball.age;
        std::optional<Vec3d> impact;
        for (const EndFightPlayer& p : players_) {
            if (p.alive && player_box(p.feet).inflated(0.5).contains(next)) {
                impact = p.feet;  // measured: the cloud stands at the player's feet
                break;
            }
        }
        if (!impact) {
            const f64 length = fireball.velocity.length();
            if (length > 0.0) {
                const auto hit = raycast_voxels(
                    fireball.position, fireball.velocity, length, [&](BlockPos cell) {
                        const registry::BlockStateId state = level.block_at(cell);
                        return state != registry::kAirState && !transparent(state) &&
                               blocks_->blocks_motion(blocks_->block_of(state));
                    });
                if (hit) {
                    impact = hit->position;
                }
            }
        }
        if (impact || fireball.age > 1200 || next.y < -64.0) {
            host.broadcast(net::clientbound::kRemoveEntities,
                           net::encode_remove_entity(fireball.entity_id));
            if (impact) {
                host.broadcast(net::clientbound::kWorldEvent,
                               net::encode_world_event(kEventFireballImpact, wire(*impact), 1, false));
                spawn_cloud(*impact, false, host);
            }
            fireballs_.erase(fireballs_.begin() + static_cast<isize>(i));
            continue;
        }
        fireball.position = next;
        host.broadcast(net::clientbound::kEntityTeleport,
                       net::encode_entity_teleport(fireball.entity_id, next.x, next.y, next.z, 0.0F,
                                                   0.0F, false));
        ++i;
    }
}

void EndFight::spawn_cloud(Vec3d at, bool breath, const EndFightHost& host) {
    CloudEntity cloud;
    cloud.entity_id = host.reserve_entity_ids(1);
    cloud.uuid      = next_uuid();
    cloud.position  = at;
    cloud.cloud     = breath ? breath_cloud() : fireball_cloud();
    cloud.amplifier = breath ? 0 : 1;
    cloud.breath    = breath;
    cloud.sent_radius  = cloud.cloud.radius;
    cloud.sent_waiting = true;
    cloud.reapply.reserve(8);
    clouds_.push_back(std::move(cloud));
    send_cloud(clouds_.back(), host.broadcast);
}

void EndFight::tick_clouds(const EndFightHost& host) {
    for (usize i = 0; i < clouds_.size();) {
        CloudEntity& cloud = clouds_[i];
        if (!gameplay::cloud_tick(cloud.cloud)) {
            host.broadcast(net::clientbound::kRemoveEntities,
                           net::encode_remove_entity(cloud.entity_id));
            clouds_.erase(clouds_.begin() + static_cast<isize>(i));
            continue;
        }
        const bool waiting = gameplay::cloud_waiting(cloud.cloud);
        if (waiting != cloud.sent_waiting || (!waiting && cloud.cloud.radius != cloud.sent_radius)) {
            net::MetadataWriter fields;
            if (!waiting && cloud.cloud.radius != cloud.sent_radius) {
                fields.float_value(kCloudRadius, cloud.cloud.radius);
            }
            if (waiting != cloud.sent_waiting) {
                fields.boolean_value(kCloudWaiting, waiting);
            }
            cloud.sent_radius  = cloud.cloud.radius;
            cloud.sent_waiting = waiting;
            host.broadcast(net::clientbound::kEntityMetadata,
                           net::encode_entity_metadata(cloud.entity_id, fields.take()));
        }
        if (gameplay::cloud_scans(cloud.cloud) && host.hurt_player) {
            // Harming at half strength, once a second per player: 3 for the
            // breath, 6 for a fireball's cloud (wiki). It does not shrink.
            const f64 harm = 6.0 * static_cast<f64>(1 << cloud.amplifier) *
                             gameplay::kCloudInstantFactor;
            for (const EndFightPlayer& p : players_) {
                if (!p.mortal || !p.alive) {
                    continue;
                }
                const Vec3d d = p.feet - cloud.position;
                if (!gameplay::cloud_reaches(cloud.cloud, d.x, d.y, d.z, kPlayerHeight)) {
                    continue;
                }
                auto next = std::ranges::find_if(cloud.reapply,
                                                 [&](const auto& e) { return e.first == p.id; });
                if (next != cloud.reapply.end() && cloud.cloud.age < next->second) {
                    continue;
                }
                const i32 again = cloud.cloud.age + cloud.cloud.reapplication_delay;
                if (next == cloud.reapply.end()) {
                    cloud.reapply.emplace_back(p.id, again);
                } else {
                    next->second = again;
                }
                (void)host.hurt_player(p.id, static_cast<f32>(harm),
                                       gameplay::DamageKind::IndirectMagic);
            }
        }
        ++i;
    }
}

void EndFight::spawn_orbs(Vec3d at, i32 amount, const EndFightHost& host) {
    std::array<i32, 64> values{};
    const usize         count = gameplay::split_into_orbs(amount, values);
    for (usize k = 0; k < count; ++k) {
        Orb orb;
        orb.entity_id = host.reserve_entity_ids(1);
        orb.position  = at;
        const f64 vx  = random_.next_double() * 0.2 - 0.1;
        const f64 vy  = random_.next_double() * 0.2;
        const f64 vz  = random_.next_double() * 0.2 - 0.1;
        orb.velocity  = Vec3d{vx * 2.0, vy * 2.0, vz * 2.0};
        orb.value     = values[k];
        host.broadcast(net::clientbound::kSpawnExperienceOrb,
                       net::encode_spawn_experience_orb(orb.entity_id, at.x, at.y, at.z,
                                                        static_cast<i16>(orb.value)));
        orbs_.push_back(orb);
    }
}

void EndFight::tick_orbs(const world::LevelView& level, const EndFightHost& host) {
    const gameplay::OrbConstants rules{};
    for (usize i = 0; i < orbs_.size();) {
        Orb& orb = orbs_[i];
        ++orb.age;
        const EndFightPlayer* taker   = nullptr;
        f64                   nearest = rules.follow_range * rules.follow_range;
        for (const EndFightPlayer& p : players_) {
            if (!p.alive) {
                continue;
            }
            const Vec3d d = Vec3d{p.feet.x, p.feet.y + 0.9, p.feet.z} - orb.position;
            if (d.length_squared() < nearest) {
                nearest = d.length_squared();
                taker   = &p;
            }
        }
        if (taker != nullptr && nearest <= rules.pickup_range * rules.pickup_range) {
            if (host.award_experience) {
                host.award_experience(taker->id, orb.value);
            }
            host.broadcast(net::clientbound::kTakeItem,
                           net::encode_take_item(orb.entity_id, taker->id, 1));
            host.broadcast(net::clientbound::kRemoveEntities,
                           net::encode_remove_entity(orb.entity_id));
            orbs_.erase(orbs_.begin() + static_cast<isize>(i));
            continue;
        }
        if (orb.age >= rules.lifetime_ticks) {
            host.broadcast(net::clientbound::kRemoveEntities,
                           net::encode_remove_entity(orb.entity_id));
            orbs_.erase(orbs_.begin() + static_cast<isize>(i));
            continue;
        }
        if (taker != nullptr) {
            const Vec3d d = Vec3d{taker->feet.x, taker->feet.y + 0.9, taker->feet.z} - orb.position;
            const f64 distance = std::sqrt(nearest);
            const f64 pull     = rules.follow_speed * (1.0 - distance / rules.follow_range);
            orb.velocity += d * (pull / std::max(distance, 1e-3));
        } else {
            orb.velocity.y -= 0.03;
        }
        Vec3d          next = orb.position + orb.velocity;
        const BlockPos cell = block_at(next);
        const registry::BlockStateId state = level.block_at(cell);
        if (state != registry::kAirState && blocks_->blocks_motion(blocks_->block_of(state))) {
            next.y         = static_cast<f64>(cell.y) + 1.0;
            orb.velocity.y = 0.0;
            orb.velocity.x *= 0.6;
            orb.velocity.z *= 0.6;
        }
        orb.velocity = orb.velocity * 0.98;
        if ((next - orb.position).length_squared() > 1e-6) {
            orb.position = next;
            host.broadcast(net::clientbound::kEntityTeleport,
                           net::encode_entity_teleport(orb.entity_id, next.x, next.y, next.z, 0.0F,
                                                       0.0F, false));
        }
        ++i;
    }
}

// ── Death ───────────────────────────────────────────────────────────────────

void EndFight::finish_death(world::LevelWriter& level, const EndFightHost& host, bool animation) {
    if (!dragon_) {
        return;
    }
    host.broadcast(net::clientbound::kRemoveEntities, net::encode_remove_entity(dragon_id_));
    host.broadcast(kBossBarPacket, bar_packet(bar_uuid_, kBarRemove));
    dragon_.reset();
    killed_  = true;
    nearest_ = -1;

    // The game's order: a gateway, the portal opened, the egg.
    if (!gateways_.empty()) {
        const i32 index = gateways_.back();
        gateways_.pop_back();
        const BlockPos gateway = gameplay::end_gateway_position(index);
        rules_->build_gateway(level, gateway);
        last_gateway_ = gateway;
        host.broadcast(net::clientbound::kWorldEvent,
                       net::encode_world_event(kEventGatewaySpawn,
                                               net::WirePosition{gateway.x, gateway.y, gateway.z},
                                               0, false));
    }
    if (portal_) {
        rules_->build_exit_portal(level, *portal_, true);
        if (!previously_killed_) {
            level.set_block(BlockPos{portal_->x, portal_->y + 4, portal_->z}, rules_->dragon_egg());
        }
    }
    previously_killed_ = true;
    OV_LOG_INFO("end: the dragon is dead{} — the exit portal is open{}",
                animation ? "" : " (killed outright)", last_gateway_ ? " and a gateway with it" : "");
}

// ── The respawn ─────────────────────────────────────────────────────────────

void EndFight::rebuild_spike(world::LevelWriter& level, usize index) const {
    // The feature's own shape (worldgen end_features.cpp), rebuilt over what
    // is there: obsidian inside the disc up to the height, air above y 65 in
    // the box, the cage for the guarded two, bedrock on top.
    const auto               spikes = worldgen::end_spikes(seed_);
    const worldgen::EndSpike spike  = spikes[index];
    const i32                r      = spike.radius;
    const world::WorldShape  shape  = level.shape();
    for (i32 z = spike.centre_z - r; z <= spike.centre_z + r; ++z) {
        for (i32 y = shape.min_y; y <= spike.height + 10; ++y) {
            for (i32 x = spike.centre_x - r; x <= spike.centre_x + r; ++x) {
                const f64 dx = static_cast<f64>(x - spike.centre_x);
                const f64 dz = static_cast<f64>(z - spike.centre_z);
                if (dx * dx + dz * dz <= static_cast<f64>(r * r + 1) && y < spike.height) {
                    level.set_block(BlockPos{x, y, z}, obsidian_);
                } else if (y > 65) {
                    level.set_block(BlockPos{x, y, z}, registry::kAirState);
                }
            }
        }
    }
    if (spike.guarded && iron_bars_ != registry::kAirState) {
        const auto with = [&](registry::BlockStateId state, std::string_view name, bool value) {
            const auto property = blocks_->find_property(blocks_->block_of(state), name);
            if (!property) {
                return state;
            }
            for (usize i = 0; i < property->values.size(); ++i) {
                if (property->values[i] == (value ? "true" : "false")) {
                    return blocks_->with_property(state, *property, static_cast<u16>(i));
                }
            }
            return state;
        };
        for (i32 j = -2; j <= 2; ++j) {
            for (i32 k = -2; k <= 2; ++k) {
                for (i32 l = 0; l <= 3; ++l) {
                    const bool edge_x = std::abs(j) == 2;
                    const bool edge_z = std::abs(k) == 2;
                    const bool roof   = l == 3;
                    if (!edge_x && !edge_z && !roof) {
                        continue;
                    }
                    const bool along_x = j == -2 || j == 2 || roof;
                    const bool along_z = k == -2 || k == 2 || roof;
                    auto       bars    = with(iron_bars_, "north", along_x && k != -2);
                    bars               = with(bars, "south", along_x && k != 2);
                    bars               = with(bars, "west", along_z && j != -2);
                    bars               = with(bars, "east", along_z && j != 2);
                    level.set_block(
                        BlockPos{spike.centre_x + j, spike.height + l, spike.centre_z + k}, bars);
                }
            }
        }
    }
    level.set_block(BlockPos{spike.centre_x, spike.height, spike.centre_z}, bedrock_);
}

void EndFight::tick_respawn(world::LevelWriter& level, const EndFightHost& host) {
    if (respawn_check_) {
        respawn_check_ = false;
        if (killed_ && !dragon_ && respawn_ticks_ < 0 && portal_) {
            // One placed crystal on each side of the exit portal's rim: within
            // a block of the side's middle, three blocks out, one up.
            constexpr std::array<std::array<i32, 2>, 4> kSides{
                std::array<i32, 2>{3, 0}, {-3, 0}, {0, 3}, {0, -3}};
            std::array<usize, 4> found{};
            bool                 all = true;
            for (usize s = 0; s < kSides.size() && all; ++s) {
                const Vec3d want{static_cast<f64>(portal_->x + kSides[s][0]) + 0.5,
                                 static_cast<f64>(portal_->y) + 1.0,
                                 static_cast<f64>(portal_->z + kSides[s][1]) + 0.5};
                bool here = false;
                for (usize i = 0; i < crystals_.size() && !here; ++i) {
                    const Crystal& c = crystals_[i];
                    if (c.alive && c.placed && std::abs(c.position.x - want.x) <= 1.5 &&
                        std::abs(c.position.z - want.z) <= 1.5 &&
                        std::abs(c.position.y - want.y) <= 1.0) {
                        found[s] = i;
                        here     = true;
                    }
                }
                all = here;
            }
            if (all) {
                respawn_ticks_    = 0;
                respawn_crystals_ = found;
                // The exit portal goes back to its inactive shape and the egg
                // goes with it (wiki).
                rules_->build_exit_portal(level, *portal_, false);
                level.set_block(BlockPos{portal_->x, portal_->y + 4, portal_->z},
                                registry::kAirState);
                OV_LOG_INFO("end: four crystals on the exit portal — the dragon is being respawned");
            }
        }
    }
    if (respawn_ticks_ < 0) {
        return;
    }
    ++respawn_ticks_;
    const auto beam_all = [&](std::optional<BlockPos> target) {
        for (const usize index : respawn_crystals_) {
            Crystal& c = crystals_[index];
            c.beam     = target;
            net::MetadataWriter fields;
            fields.optional_block_pos_value(
                kCrystalBeam, target ? std::optional<net::WirePosition>{net::WirePosition{
                                           target->x, target->y, target->z}}
                                     : std::nullopt);
            host.broadcast(net::clientbound::kEntityMetadata,
                           net::encode_entity_metadata(c.entity_id, fields.take()));
        }
    };
    const auto spikes = worldgen::end_spikes(seed_);
    const auto set_beam = [&](Crystal& c, std::optional<BlockPos> target) {
        c.beam = target;
        net::MetadataWriter fields;
        fields.optional_block_pos_value(
            kCrystalBeam, target ? std::optional<net::WirePosition>{net::WirePosition{
                                       target->x, target->y, target->z}}
                                 : std::nullopt);
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(c.entity_id, fields.take()));
    };
    if (respawn_ticks_ == 1) {
        beam_all(BlockPos{0, 128, 0});
    }
    const i32 into = respawn_ticks_ - kRespawnPillarsStart;
    if (into >= 0 && into / kRespawnPillarStep < static_cast<i32>(spikes.size())) {
        const usize              k     = static_cast<usize>(into / kRespawnPillarStep);
        const worldgen::EndSpike spike = spikes[k];
        const Vec3d              crystal_at{static_cast<f64>(spike.centre_x) + 0.5,
                               static_cast<f64>(spike.height) + 1.0,
                               static_cast<f64>(spike.centre_z) + 0.5};
        if (into % kRespawnPillarStep == 0) {
            // The four point at the pillar's crystal block.
            beam_all(BlockPos{spike.centre_x, spike.height + 1, spike.centre_z});
        } else if (into % kRespawnPillarStep == kRespawnPillarBlast) {
            // The crystal there goes, the top of the pillar blows (destroying
            // what players left there), the pillar is rebuilt, a crystal
            // appears — its beam on (0, 128, 0). All measured.
            for (usize i = 0; i < crystals_.size(); ++i) {
                Crystal& c = crystals_[i];
                if (c.alive && !c.placed && (c.position - crystal_at).length_squared() < 1.0) {
                    c.alive = false;
                    host.broadcast(net::clientbound::kRemoveEntities,
                                   net::encode_remove_entity(c.entity_id));
                }
            }
            blasts_.push_back(PendingBlast{Vec3d{crystal_at.x, crystal_at.y - 1.0, crystal_at.z},
                                           std::nullopt, kRespawnPillarPower, false});
            rebuild_spike(level, k);
            Crystal crystal;
            crystal.entity_id = host.reserve_entity_ids(1);
            crystal.uuid      = next_uuid();
            crystal.position  = crystal_at;
            crystal.beam      = BlockPos{0, 128, 0};
            crystals_.push_back(crystal);
            send_crystal(crystals_.back(), host.broadcast);
        }
    }
    if (respawn_ticks_ == kRespawnSummon) {
        beam_all(BlockPos{0, 128, 0});
    }
    if (respawn_ticks_ >= kDragonRespawnTicks) {
        // The dragon; the four crystals blow without touching a block; every
        // beam goes out; and the dragon, told of four crystals destroyed,
        // turns on the nearest player (measured: holding, then strafe).
        respawn_ticks_ = -1;
        killed_        = false;
        dragon_uuid_.reset();
        crystals_.erase(std::remove_if(crystals_.begin(), crystals_.end(),
                                       [](const Crystal& c) { return !c.alive; }),
                        crystals_.end());
        spawn_dragon(host);
        net::SpawnEntity spawn;
        spawn.entity_id = dragon_id_;
        spawn.uuid      = *dragon_uuid_;
        spawn.type      = types_.dragon;
        spawn.x         = 0.0;
        spawn.y         = 128.0;
        spawn.z         = 0.0;
        spawn.yaw       = dragon_->wire_yaw();
        spawn.head_yaw  = dragon_->wire_yaw();
        host.broadcast(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
        net::MetadataWriter fields;
        fields.float_value(kMetadataHealth, kDragonMaxHealth);
        fields.varint_value(kMetadataPhase, static_cast<i32>(DragonPhase::HoldingPattern));
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(dragon_id_, fields.take()));
        host.broadcast(kBossBarPacket, boss_bar_add());
        std::optional<Vec3d> last;
        for (Crystal& c : crystals_) {
            if (c.placed && c.alive) {
                c.alive = false;
                host.broadcast(net::clientbound::kRemoveEntities,
                               net::encode_remove_entity(c.entity_id));
                blasts_.push_back(PendingBlast{c.position, std::nullopt,
                                               gameplay::kEndCrystalPower, true});
                last = c.position;
            }
        }
        for (Crystal& c : crystals_) {
            if (c.alive && c.beam) {
                set_beam(c, std::nullopt);
            }
        }
        recount_crystals();
        if (last) {
            dragon_->crystal_destroyed(strafe_target(std::nullopt, true, *last));
        }
        OV_LOG_INFO("end: the dragon is back");
    }
}

}  // namespace ov::server
