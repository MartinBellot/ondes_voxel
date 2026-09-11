#define OV_LOG_CATEGORY "server"

#include "end_fight.hpp"

#include "ov/base/log.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/worldgen/end.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

namespace ov::server {

namespace {

/// Boss Bar actions.
constexpr i32 kBarAdd          = 0;
constexpr i32 kBarRemove       = 1;
constexpr i32 kBarUpdateHealth = 2;
/// Pink, a plain bar; the boss music (0x02) and the world fog (0x04).
constexpr i32 kBarPink     = 0;
constexpr i32 kBarProgress = 0;
constexpr u8  kBarFlags    = 0x02 | 0x04;
constexpr std::string_view kBarTitle = R"({"translate":"entity.minecraft.ender_dragon"})";

/// Living entity health, and the dragon's phase: 0 circling, 9 dying.
constexpr u8  kMetadataHealth = 9;
constexpr u8  kMetadataPhase  = 16;
constexpr i32 kPhaseHolding   = 0;
constexpr i32 kPhaseDying     = 9;

/// The circle this server flies the dragon on — not the game's (see the header).
constexpr f64 kOrbitRadius = 60.0;
constexpr f64 kOrbitHeight = 90.0;
constexpr f64 kOrbitStep   = 0.0125;  // radians per tick: 0.75 blocks per tick

/// World events: the dragon's death (global) and a gateway opening.
constexpr i32 kEventDragonDeath   = 1028;
constexpr i32 kEventGatewaySpawn  = 3000;

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

}  // namespace

EndFight::EndFight(const gameplay::EndPortalRules& rules, i64 seed, i32 dragon_type,
                   i32 crystal_type)
    : rules_(&rules),
      seed_(seed),
      dragon_type_(dragon_type),
      crystal_type_(crystal_type),
      random_(seed ^ 0x454E44L) {}

usize EndFight::crystals_alive() const noexcept {
    return static_cast<usize>(std::ranges::count_if(crystals_, [](const Crystal& c) {
        return c.alive && c.entity_id != 0;
    }));
}

bool EndFight::within_range(Vec3d position) noexcept {
    const f64 dx = position.x;
    const f64 dy = position.y - 128.0;
    const f64 dz = position.z;
    return dx * dx + dy * dy + dz * dz <= 192.0 * 192.0;
}

void EndFight::start(world::LevelWriter& level, i32 origin_top, const EndFightHost& host) {
    if (started_) {
        return;
    }
    started_ = true;
    portal_  = rules_->exit_portal_origin(level, origin_top);
    rules_->build_exit_portal(level, *portal_, false);

    // Nine ids: the dragon and its eight parts, which the client numbers
    // after it — a hit on the head arrives with the dragon's id plus one.
    dragon_id_    = host.reserve_entity_ids(9);
    dragon_uuid_  = uuid_from(random_);
    bar_uuid_     = uuid_from(random_);
    dragon_       = Vec3d{0.0, 128.0, 0.0};
    health_       = kDragonMaxHealth;
    dragon_alive_ = true;

    const auto spikes = worldgen::end_spikes(seed_);
    for (usize i = 0; i < spikes.size(); ++i) {
        Crystal& crystal  = crystals_[i];
        crystal.entity_id = host.reserve_entity_ids(1);
        crystal.uuid      = uuid_from(random_);
        crystal.position  = Vec3d{static_cast<f64>(spikes[i].centre_x) + 0.5,
                                 static_cast<f64>(spikes[i].height) + 1.0,
                                 static_cast<f64>(spikes[i].centre_z) + 0.5};
        crystal.alive     = true;
    }
    OV_LOG_INFO("end: the dragon fight starts — exit portal at ({}, {}, {}), the dragon at "
                "(0, 128, 0), {} crystals",
                portal_->x, portal_->y, portal_->z, crystals_.size());
}

std::vector<u8> EndFight::boss_bar_add() const {
    io::ByteWriter writer;
    writer.write_u64(bar_uuid_.most_significant);
    writer.write_u64(bar_uuid_.least_significant);
    net::write_varint(writer, kBarAdd);
    net::write_varint(writer, static_cast<i32>(kBarTitle.size()));
    writer.write_bytes(kBarTitle);
    writer.write_f32(health_ / kDragonMaxHealth);
    net::write_varint(writer, kBarPink);
    net::write_varint(writer, kBarProgress);
    writer.write_u8(kBarFlags);
    return writer.take();
}

void EndFight::show_to(const std::function<void(i32, std::span<const u8>)>& send) const {
    if (!started_) {
        return;
    }
    if (dragon_alive_) {
        net::SpawnEntity spawn;
        spawn.entity_id = dragon_id_;
        spawn.uuid      = dragon_uuid_;
        spawn.type      = dragon_type_;
        spawn.x         = dragon_.x;
        spawn.y         = dragon_.y;
        spawn.z         = dragon_.z;
        spawn.yaw       = yaw_;
        spawn.head_yaw  = yaw_;
        send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
        net::MetadataWriter fields;
        fields.float_value(kMetadataHealth, health_);
        fields.varint_value(kMetadataPhase, health_ > 0.0F ? kPhaseHolding : kPhaseDying);
        send(net::clientbound::kEntityMetadata,
             net::encode_entity_metadata(dragon_id_, fields.take()));
        send(kBossBarPacket, boss_bar_add());
    }
    for (const Crystal& crystal : crystals_) {
        if (!crystal.alive || crystal.entity_id == 0) {
            continue;
        }
        net::SpawnEntity spawn;
        spawn.entity_id = crystal.entity_id;
        spawn.uuid      = crystal.uuid;
        spawn.type      = crystal_type_;
        spawn.x         = crystal.position.x;
        spawn.y         = crystal.position.y;
        spawn.z         = crystal.position.z;
        send(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
    }
}

void EndFight::hide_from(const std::function<void(i32, std::span<const u8>)>& send) const {
    if (started_) {
        send(kBossBarPacket, bar_packet(bar_uuid_, kBarRemove));
    }
}

void EndFight::set_health(f32 health, const EndFightHost& host) {
    health = std::clamp(health, 0.0F, kDragonMaxHealth);
    if (health == health_) {
        return;
    }
    health_ = health;
    net::MetadataWriter fields;
    fields.float_value(kMetadataHealth, health_);
    host.broadcast(net::clientbound::kEntityMetadata,
                   net::encode_entity_metadata(dragon_id_, fields.take()));
    auto bar = bar_packet(bar_uuid_, kBarUpdateHealth);
    io::ByteWriter tail;
    tail.write_f32(health_ / kDragonMaxHealth);
    bar.insert(bar.end(), tail.data().begin(), tail.data().end());
    host.broadcast(kBossBarPacket, bar);
}

void EndFight::tick(world::LevelWriter& level, const EndFightHost& host) {
    if (!started_ || !dragon_alive_) {
        return;
    }
    ++ticks_;

    if (health_ <= 0.0F) {
        // Dying: two hundred ticks rising slowly, then the portal.
        ++death_time_;
        dragon_.y += 0.1;
        host.broadcast(net::clientbound::kEntityTeleport,
                       net::encode_entity_teleport(dragon_id_, dragon_.x, dragon_.y, dragon_.z,
                                                   yaw_, 0.0F, false));
        if (death_time_ >= kDragonDeathTicks) {
            finish_death(level, host);
        }
        return;
    }

    // The flight: a circle round the island (named in the header).
    angle_ += kOrbitStep;
    const f64 x  = kOrbitRadius * std::cos(angle_);
    const f64 z  = kOrbitRadius * std::sin(angle_);
    const f64 dx = x - dragon_.x;
    const f64 dz = z - dragon_.z;
    dragon_      = Vec3d{x, kOrbitHeight, z};
    yaw_ = static_cast<f32>(std::atan2(dz, dx) * 180.0 / std::numbers::pi) - 90.0F;
    host.broadcast(net::clientbound::kEntityTeleport,
                   net::encode_entity_teleport(dragon_id_, dragon_.x, dragon_.y, dragon_.z, yaw_,
                                               0.0F, false));

    // The crystals: the one being drawn on heals a point every ten ticks; a
    // new one is looked for one tick in ten, within 32 of the dragon's box
    // (16 wide, 8 tall).
    if (nearest_ >= 0) {
        if (!crystals_[static_cast<usize>(nearest_)].alive) {
            nearest_ = -1;
        } else if (ticks_ % 10 == 0 && health_ < kDragonMaxHealth) {
            set_health(health_ + 1.0F, host);
        }
    }
    if (random_.next_int(10) == 0) {
        f64 best = 0.0;
        nearest_ = -1;
        for (usize i = 0; i < crystals_.size(); ++i) {
            const Crystal& crystal = crystals_[i];
            if (!crystal.alive || crystal.entity_id == 0) {
                continue;
            }
            const Vec3d& p = crystal.position;
            if (std::abs(p.x - dragon_.x) > 40.0 || std::abs(p.z - dragon_.z) > 40.0 ||
                p.y < dragon_.y - 32.0 || p.y > dragon_.y + 40.0) {
                continue;
            }
            const f64 d = (p.x - dragon_.x) * (p.x - dragon_.x) +
                          (p.y - dragon_.y) * (p.y - dragon_.y) +
                          (p.z - dragon_.z) * (p.z - dragon_.z);
            if (nearest_ < 0 || d < best) {
                best     = d;
                nearest_ = static_cast<i32>(i);
            }
        }
    }
}

bool EndFight::hurt(i32 entity_id, f32 damage, const EndFightHost& host) {
    if (!started_) {
        return false;
    }
    for (usize i = 0; i < crystals_.size(); ++i) {
        Crystal& crystal = crystals_[i];
        if (crystal.entity_id != entity_id || crystal.entity_id == 0) {
            continue;
        }
        if (crystal.alive) {
            crystal.alive = false;
            const std::array<i32, 1> gone{crystal.entity_id};
            host.broadcast(net::clientbound::kRemoveEntities, net::encode_remove_entities(gone));
            if (nearest_ == static_cast<i32>(i)) {
                nearest_ = -1;
            }
            OV_LOG_INFO("end: a crystal is destroyed, {} left (its explosion is not simulated)",
                        crystals_alive());
        }
        return true;
    }
    if (entity_id < dragon_id_ || entity_id > dragon_id_ + 8) {
        return false;
    }
    if (!dragon_alive_ || health_ <= 0.0F) {
        return true;
    }
    // Part 0 is the head: full damage. Every other part, and the body.
    const i32 part = entity_id - dragon_id_ - 1;
    if (part != 0) {
        damage = damage / 4.0F + std::min(damage, 1.0F);
    }
    if (damage < 0.01F) {
        return true;
    }
    set_health(health_ - damage, host);
    if (health_ <= 0.0F) {
        net::MetadataWriter fields;
        fields.varint_value(kMetadataPhase, kPhaseDying);
        host.broadcast(net::clientbound::kEntityMetadata,
                       net::encode_entity_metadata(dragon_id_, fields.take()));
        host.broadcast(net::clientbound::kWorldEvent,
                       net::encode_world_event(kEventDragonDeath,
                                               net::WirePosition{static_cast<i32>(dragon_.x),
                                                                 static_cast<i32>(dragon_.y),
                                                                 static_cast<i32>(dragon_.z)},
                                               0, true));
        OV_LOG_INFO("end: the dragon is dying");
    }
    return true;
}

void EndFight::finish_death(world::LevelWriter& level, const EndFightHost& host) {
    dragon_alive_ = false;
    killed_       = true;
    const std::array<i32, 1> gone{dragon_id_};
    host.broadcast(net::clientbound::kRemoveEntities, net::encode_remove_entities(gone));
    host.broadcast(kBossBarPacket, bar_packet(bar_uuid_, kBarRemove));

    // The game's order: a gateway, the portal opened, the egg.
    const auto order = gameplay::end_gateway_order(seed_);
    if (gateways_opened_ < order.size()) {
        const BlockPos gateway = order[gateways_opened_++];
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
    OV_LOG_INFO("end: the dragon is dead — the exit portal is open{}",
                last_gateway_ ? " and a gateway with it" : "");
}

}  // namespace ov::server
