// ── nether-2 ── The Nether's mobs: a world of their own. See the header.
#define OV_LOG_CATEGORY "server"

#include "nether_mobs.hpp"

#include "entity_nbt.hpp"  // ── persistence ──
#include "natural_spawning.hpp"
#include "survival_session.hpp"  // damage_type_id

#include "ov/base/log.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/gameplay/enchanting.hpp"
#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/blast.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/world/chunk.hpp"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ov::server {
namespace {

/// The same derivation the server uses for its own mobs: SplitMix64 over the
/// wire id, so a replay gives the same uuids and no two entities share one.
[[nodiscard]] net::Uuid uuid_for(i32 network_id) {
    const auto mix = [](u64 z) {
        z += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    const auto id = static_cast<u64>(static_cast<u32>(network_id));
    return net::Uuid{mix(id), mix(id ^ 0xA5A5A5A5A5A5A5A5ULL)};
}

// Metadata indices (Entity metadata, protocol 763 — the Mob's fields end at
// 15; each type's own start at 16). Named here so a wire capture can check
// them one by one (docs/provenance/nether-2.md § 3.5).
constexpr u8 kBlazeFlags       = 16;  // byte, 0x01: on fire (charging)
constexpr u8 kGhastAttacking   = 16;  // boolean
constexpr u8 kStriderShaking   = 18;  // boolean (17 is the boost time)
constexpr u8 kEquipmentMain    = 0;
constexpr u8 kEquipmentOffhand = 1;

/// Set Equipment: the entity, then (slot | 0x80 while more follow, item).
[[nodiscard]] std::vector<u8> encode_equipment(
    i32 entity, std::span<const std::pair<u8, net::ItemStack>> slots) {
    io::ByteWriter writer;
    net::write_varint(writer, entity);
    for (usize i = 0; i < slots.size(); ++i) {
        const u8 more = i + 1 < slots.size() ? u8{0x80} : u8{0};
        writer.write_u8(static_cast<u8>(slots[i].first | more));
        net::write_slot(writer, slots[i].second);
    }
    return writer.take();
}

/// The Nether's blocks, as a goal and a spawner read them.
class NetherLevelView final : public world::LevelView {
public:
    NetherLevelView(const registry::BlockRegistry& blocks) : blocks_(blocks) {}

    void attach(const NetherMobHost* host) noexcept { host_ = host; }

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        if (host_ == nullptr || !host_->block_at || pos.y < 0 || pos.y >= 256) {
            return registry::kAirState;
        }
        return host_->block_at(pos);
    }
    [[nodiscard]] bool is_loaded(BlockPos pos) const override {
        return host_ != nullptr && host_->loaded && host_->loaded(pos);
    }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::nether(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return blocks_; }

    static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
        return static_cast<const NetherLevelView*>(context)->block_at(BlockPos{x, y, z});
    }

private:
    const registry::BlockRegistry& blocks_;
    const NetherMobHost*           host_{nullptr};
};

struct Fireball {
    entity::EntityHandle handle{entity::kNoEntity};
    i32                  owner{0};
    Vec3d                power{};
    bool                 large{false};
    i32                  age{0};
};

struct Proxy {
    i32                  player{0};
    entity::EntityHandle any{entity::kNoEntity};     // type player: every hostile's quarry
    entity::EntityHandle piglin{entity::kNoEntity};  // type villager: a piglin's, gold-less only
    bool                 seen{false};
};

struct Admiring {
    i64 until{0};
    i32 player{0};
};

struct Anger {
    i64                  until{0};
    entity::EntityHandle proxy{entity::kNoEntity};
};

[[nodiscard]] f64 dist_sq(Vec3d a, Vec3d b) {
    const f64 dx = a.x - b.x;
    const f64 dy = a.y - b.y;
    const f64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

struct NetherMobs::Impl {
    Impl(const registry::Registries& registries_, const registry::BlockRegistry& blocks_,
         MobCombat* combat_, const gameplay::LootTables* block_loot_, i64 seed)
        : registries(registries_), blocks(blocks_), combat(combat_), block_loot(block_loot_),
          world(registries_, kFirstId), spawner(static_cast<u64>(seed) ^ 0x4E45'5448'4552ULL),
          explosions(blocks_), magma(registries_, "minecraft:magma_cube"), view(blocks_),
          collisions(blocks_, &NetherLevelView::look_up, &view),
          light([this] {
              LightHooks hooks;
              hooks.block_light = [this](BlockPos pos) -> u8 {
                  return host != nullptr && host->block_light ? host->block_light(pos) : u8{0};
              };
              hooks.sky_light  = [](BlockPos) -> u8 { return 0; };
              hooks.sky_darken = []() -> u8 { return 0; };
              return hooks;
          }()),
          biomes([this](BlockPos pos) -> u16 {
              return host != nullptr && host->biome_at ? host->biome_at(pos) : u16{0};
          }),
          random(seed ^ 0x6E65'7468'6572LL) {}

    const registry::Registries&    registries;
    const registry::BlockRegistry& blocks;
    MobCombat*                     combat;
    const gameplay::LootTables*    block_loot;

    entity::EntityWorld      world;
    gameplay::NaturalSpawner spawner;
    std::vector<std::string> spawner_names;
    gameplay::BarterTable    barter;
    gameplay::Explosions     explosions;
    gameplay::Detonation     detonation;
    Slimes                   magma;
    NetherLevelView          view;
    gameplay::CollisionWorld collisions;
    ChunkLight               light;
    ChunkBiomes              biomes;
    const NetherMobHost*     host{nullptr};

    math::LegacyRandomSource    random;
    math::XoroshiroRandomSource loot_random{0x0E7E'0A57ULL, 0xBA27'E200ULL};
    gameplay::DamageConstants   damage_constants{};

    std::optional<registry::RegistryId> entity_registry;
    std::optional<registry::RegistryId> item_registry;
    i32 player_type{-1}, villager_type{-1};
    i32 small_fireball_type{-1}, fireball_type{-1};
    i32 piglin_type{-1}, brute_type{-1}, zombified_type{-1}, blaze_type{-1}, ghast_type{-1};
    i32 strider_type{-1}, wither_skeleton_type{-1}, magma_type{-1};
    registry::BlockId tnt_block{0};
    std::optional<registry::BlockStateId> fire_state;
    std::optional<registry::BlockStateId> soul_fire_state;

    std::unordered_map<i32, gameplay::BlazeVolley> volleys;
    std::unordered_map<i32, gameplay::GhastCharge> charges;
    std::unordered_map<i32, bool>                  shaking;
    std::unordered_map<i32, Admiring>              admiring;
    std::unordered_map<i32, Anger>                 anger;
    std::unordered_map<i32, net::ItemStack>        main_hand;
    std::vector<Fireball>                          fireballs;
    std::vector<Proxy>                             proxies;
    std::vector<gameplay::SlimeChild>              births;
    std::vector<gameplay::SlimeChild>              birth_scratch;

    std::vector<NetherPlayer>           players;
    std::vector<Vec3d>                  spawn_players;
    std::vector<ChunkPos>               spawn_ticking;
    std::vector<gameplay::SpawnRequest> spawn_requests;
    std::array<i32, 8>                  live{};
    i64                                 now{0};
    i64                                 ticks_run{0};

    std::mutex                        queue_mutex;
    std::vector<std::pair<i32, i32>>  interacts;
    std::vector<std::pair<i32, i32>>  interacts_now;

    NetherMobStats stats;

    [[nodiscard]] bool is_proxy(entity::EntityHandle handle) const {
        return std::ranges::any_of(proxies, [&](const Proxy& p) {
            return p.any == handle || p.piglin == handle;
        });
    }
    [[nodiscard]] bool is_fireball(i32 type) const {
        return type == small_fireball_type || type == fireball_type;
    }
    [[nodiscard]] std::string_view name_of(i32 type) const {
        return entity_registry ? registries.entry_of(*entity_registry, type) : std::string_view{};
    }
    [[nodiscard]] i32 item_id(std::string_view name) const {
        if (!item_registry) {
            return 0;
        }
        return registries.protocol_id(*item_registry, name).value_or(0);
    }
    [[nodiscard]] i32 type_id(std::string_view name) const {
        return entity_registry ? registries.protocol_id(*entity_registry, name).value_or(-1) : -1;
    }

    void deliver_all(i32 id, std::span<const u8> payload) const {
        if (host != nullptr && host->broadcast) {
            host->broadcast(id, payload);
        }
    }

    // ── Packets ─────────────────────────────────────────────────────────────

    void packets(const entity::EntityState& state,
                 const std::function<void(i32, std::span<const u8>)>& deliver) const {
        net::SpawnEntity spawn;
        spawn.entity_id = state.network_id;
        spawn.uuid      = state.uuid;
        spawn.type      = state.type;
        const Vec3d at  = state.broadcast_valid ? state.broadcast_position : state.position;
        spawn.x         = at.x;
        spawn.y         = at.y;
        spawn.z         = at.z;
        spawn.yaw       = state.yaw;
        spawn.pitch     = state.pitch;
        spawn.head_yaw  = state.head_yaw;
        if (is_fireball(state.type)) {
            // A hurting projectile carries its owner and its acceleration.
            for (const Fireball& ball : fireballs) {
                if (world.state(ball.handle) == &state) {
                    spawn.data       = ball.owner;
                    spawn.velocity_x = ball.power.x;
                    spawn.velocity_y = ball.power.y;
                    spawn.velocity_z = ball.power.z;
                }
            }
            deliver(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
            return;
        }
        deliver(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));
        net::MetadataWriter fields;
        fields.float_value(net::metadata::kHealth, state.health);
        magma.spawn_metadata(state, fields);
        if (const auto v = volleys.find(state.network_id); v != volleys.end()) {
            fields.byte_value(kBlazeFlags, v->second.charged ? i8{1} : i8{0});
        }
        if (const auto c = charges.find(state.network_id); c != charges.end()) {
            fields.boolean_value(kGhastAttacking, c->second.attacking());
        }
        if (const auto s = shaking.find(state.network_id); s != shaking.end()) {
            fields.boolean_value(kStriderShaking, s->second);
        }
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state.network_id, fields.take()));
        std::vector<std::pair<u8, net::ItemStack>> slots;
        if (const auto hand = main_hand.find(state.network_id); hand != main_hand.end()) {
            slots.emplace_back(kEquipmentMain, hand->second);
        }
        if (admiring.contains(state.network_id)) {
            slots.emplace_back(kEquipmentOffhand,
                               net::ItemStack{item_id("minecraft:gold_ingot"), 1, {}});
        }
        if (!slots.empty()) {
            deliver(net::clientbound::kEntityEquipment, encode_equipment(state.network_id, slots));
        }
    }

    void announce(const entity::EntityState& state) const {
        packets(state, [&](i32 id, std::span<const u8> payload) { deliver_all(id, payload); });
    }

    void send_offhand(i32 id, bool gold) const {
        const std::array<std::pair<u8, net::ItemStack>, 1> slot{
            {{kEquipmentOffhand,
              gold ? net::ItemStack{item_id("minecraft:gold_ingot"), 1, {}} : net::ItemStack{}}}};
        const auto payload = encode_equipment(id, slot);
        deliver_all(net::clientbound::kEntityEquipment, payload);
    }

    // ── Spawning one mob ────────────────────────────────────────────────────

    std::optional<entity::EntityHandle> spawn(std::string_view type, Vec3d at) {
        const auto spawned = world.spawn(type, at, net::Uuid{});
        if (!spawned) {
            OV_LOG_DEBUG("nether: {} cannot be spawned: {}", type, entity::to_string(spawned.error()));
            return std::nullopt;
        }
        entity::EntityState* state = world.mutable_state(*spawned);
        state->uuid               = uuid_for(state->network_id);
        state->broadcast_position = state->position;
        state->broadcast_valid    = true;
        const i32 id              = state->network_id;
        if (state->type == magma_type) {
            magma.on_spawn(*state, random, 0.0F);
        }
        behaviour(*spawned, type);
        // What they hold (minecraft.wiki: a piglin a golden sword or a
        // crossbow, even odds; a brute a golden axe; a zombified piglin a
        // golden sword; a wither skeleton a stone sword). Armour: named, not
        // drawn.
        std::string_view held;
        if (state->type == piglin_type) {
            held = random.next_boolean() ? "minecraft:crossbow" : "minecraft:golden_sword";
        } else if (state->type == brute_type) {
            held = "minecraft:golden_axe";
        } else if (state->type == zombified_type) {
            held = "minecraft:golden_sword";
        } else if (state->type == wither_skeleton_type) {
            held = "minecraft:stone_sword";
        }
        if (!held.empty()) {
            main_hand[id] = net::ItemStack{item_id(held), 1, {}};
        }
        ++stats.spawned;
        announce(*state);
        return *spawned;
    }

    /// ── persistence ── The brain and the per-mob state a species runs on:
    /// what a spawn and a read from disk both give a mob.
    void behaviour(entity::EntityHandle handle, std::string_view type) {
        const entity::EntityState* state = world.state(handle);
        const i32                  id    = state->network_id;
        if (state->type == ghast_type) {
            world.set_logic(handle, std::make_unique<gameplay::GhastFlight>(id));
            charges[id] = gameplay::GhastCharge{};
        } else if (const gameplay::MobKind* kind = gameplay::mob_kind(type)) {
            // Who a species hunts: a piglin, only players without gold (the
            // villager-typed proxy); a zombified piglin and a strider, nobody
            // until angered; every other hostile, any player.
            i32 quarry = kind->hostile ? player_type : gameplay::kNoQuarry;
            if (state->type == piglin_type || state->type == brute_type) {
                quarry = villager_type;
            }
            if (state->type == zombified_type) {
                quarry = gameplay::kNoQuarry;
            }
            world.set_logic(handle, std::make_unique<gameplay::Mob>(*kind, state->width,
                                                                     state->height, id, quarry));
        } else {
            world.set_logic(handle, std::make_unique<gameplay::FallingMob>());
        }
        if (state->type == blaze_type) {
            volleys[id] = gameplay::BlazeVolley{};
        }
        if (state->type == strider_type) {
            shaking[id] = false;
        }
    }

    // ── Players, as the brains see them ─────────────────────────────────────

    void update_proxies() {
        for (Proxy& proxy : proxies) {
            proxy.seen = false;
        }
        for (const NetherPlayer& player : players) {
            const bool hunted = player.alive && !player.creative;
            auto       found  = std::ranges::find(proxies, player.entity_id, &Proxy::player);
            if (found == proxies.end()) {
                proxies.push_back(Proxy{player.entity_id});
                found = proxies.end() - 1;
            }
            found->seen = true;
            const auto place = [&](entity::EntityHandle& handle, i32 type, bool wanted) {
                if (!wanted) {
                    if (handle != entity::kNoEntity) {
                        (void)world.remove(handle);
                        handle = entity::kNoEntity;
                    }
                    return;
                }
                if (handle == entity::kNoEntity || world.state(handle) == nullptr) {
                    const auto made = world.spawn(type, player.feet, net::Uuid{});
                    handle          = made ? *made : entity::kNoEntity;
                }
                if (entity::EntityState* state = world.mutable_state(handle)) {
                    state->position   = player.feet;
                    state->health     = 20.0F;
                    state->max_health = 20.0F;
                }
            };
            place(found->any, player_type, hunted);
            place(found->piglin, villager_type, hunted && !player.gold_armour);
        }
        for (usize i = proxies.size(); i-- > 0;) {
            if (!proxies[i].seen) {
                if (proxies[i].any != entity::kNoEntity) (void)world.remove(proxies[i].any);
                if (proxies[i].piglin != entity::kNoEntity) (void)world.remove(proxies[i].piglin);
                proxies.erase(proxies.begin() + static_cast<isize>(i));
            }
        }
    }

    /// The nearest player a mob at `eyes` can see within `range`.
    [[nodiscard]] const NetherPlayer* sighted(Vec3d eyes, f64 range) const {
        const NetherPlayer* best = nullptr;
        f64                 best_d = range * range;
        for (const NetherPlayer& player : players) {
            if (!player.alive || player.creative) {
                continue;
            }
            const Vec3d head{player.feet.x, player.feet.y + 1.62, player.feet.z};
            const f64   d = dist_sq(eyes, head);
            if (d >= best_d || explosions.clipped(view, eyes, head)) {
                continue;
            }
            best   = &player;
            best_d = d;
        }
        return best;
    }

    // ── Shots ───────────────────────────────────────────────────────────────

    void shoot(const entity::EntityState& shooter, Vec3d from, Vec3d aim, bool large) {
        const auto spawned = world.spawn(large ? "minecraft:fireball" : "minecraft:small_fireball",
                                         from, net::Uuid{});
        if (!spawned) {
            return;
        }
        entity::EntityState* ball = world.mutable_state(*spawned);
        ball->uuid                = uuid_for(ball->network_id);
        ball->broadcast_position  = ball->position;
        ball->broadcast_valid     = true;
        fireballs.push_back(Fireball{*spawned, shooter.network_id, gameplay::fireball_power(aim),
                                     large, 0});
        ++stats.shots;
        announce(*ball);
    }

    void behave() {
        for (const entity::EntityHandle handle : world.handles()) {
            entity::EntityState* state = world.mutable_state(handle);
            if (state == nullptr || state->removed) {
                continue;
            }
            const i32 id = state->network_id;
            if (auto volley = volleys.find(id); volley != volleys.end()) {
                const Vec3d eyes{state->position.x,
                                 state->position.y + static_cast<f64>(state->eye_height),
                                 state->position.z};
                const NetherPlayer* target = sighted(eyes, 48.0);
                const bool          was    = volley->second.charged;
                if (volley->second.tick(target != nullptr) && target != nullptr) {
                    // Aimed at the middle of the target, spread by a triangle
                    // of width 2.297·√distance·½ on x and z (minecraft.wiki
                    // "Blaze": the shots scatter with distance).
                    const f64 half = static_cast<f64>(state->height) * 0.5;
                    const Vec3d from{state->position.x, state->position.y + half + 0.5,
                                     state->position.z};
                    const f64   dx   = target->feet.x - state->position.x;
                    const f64   dy   = target->feet.y + 0.9 - (state->position.y + half);
                    const f64   dz   = target->feet.z - state->position.z;
                    const f64   sqrt_d = std::sqrt(std::sqrt(dx * dx + dy * dy + dz * dz)) * 0.5;
                    const f64   sx   = random.next_gaussian() * 2.297 * sqrt_d * 0.1;
                    const f64   sz   = random.next_gaussian() * 2.297 * sqrt_d * 0.1;
                    shoot(*state, from, Vec3d{dx + sx, dy, dz + sz}, false);
                }
                if (was != volley->second.charged) {
                    net::MetadataWriter fields;
                    fields.byte_value(kBlazeFlags, volley->second.charged ? i8{1} : i8{0});
                    deliver_all(net::clientbound::kEntityMetadata,
                                net::encode_entity_metadata(id, fields.take()));
                }
            }
            if (auto charge = charges.find(id); charge != charges.end()) {
                const Vec3d eyes{state->position.x,
                                 state->position.y + static_cast<f64>(state->eye_height),
                                 state->position.z};
                const NetherPlayer* target = sighted(eyes, gameplay::kGhastSightRange);
                const bool          was    = charge->second.attacking();
                if (target != nullptr) {
                    if (auto* flight = dynamic_cast<gameplay::GhastFlight*>(world.logic(handle))) {
                        flight->face(target->feet);
                    }
                }
                if (charge->second.tick(target != nullptr) && target != nullptr) {
                    // From four blocks in front of the face, at mid-height.
                    const f64   yaw  = static_cast<f64>(state->yaw) * 3.141592653589793 / 180.0;
                    const Vec3d view_dir{-std::sin(yaw), 0.0, std::cos(yaw)};
                    const f64   half = static_cast<f64>(state->height) * 0.5;
                    const Vec3d from{state->position.x + view_dir.x * 4.0,
                                     state->position.y + half + 0.5,
                                     state->position.z + view_dir.z * 4.0};
                    const Vec3d aim{target->feet.x - from.x,
                                    target->feet.y + 0.9 - (0.5 + state->position.y + half),
                                    target->feet.z - from.z};
                    shoot(*state, from, aim, true);
                }
                if (was != charge->second.attacking()) {
                    net::MetadataWriter fields;
                    fields.boolean_value(kGhastAttacking, charge->second.attacking());
                    deliver_all(net::clientbound::kEntityMetadata,
                                net::encode_entity_metadata(id, fields.take()));
                }
            }
        }
    }

    void place_fire(BlockPos at) {
        if (host == nullptr || host->level == nullptr || !fire_state) {
            return;
        }
        if (!blocks.is_air(blocks.block_of(view.block_at(at)))) {
            return;
        }
        const std::string_view below =
            blocks.block_name(blocks.block_of(view.block_at(at.below())));
        const bool soul = below == "minecraft:soul_sand" || below == "minecraft:soul_soil";
        host->level->set_block(at, soul && soul_fire_state ? *soul_fire_state : *fire_state);
        ++stats.fires;
    }

    void explode(Vec3d centre, i32 source) {
        if (host == nullptr || host->level == nullptr) {
            return;
        }
        gameplay::ExplosionSpec spec;
        spec.centre      = centre;
        spec.power       = gameplay::kGhastFireballPower;
        spec.interaction = gameplay::BlockInteraction::Destroy;
        spec.fire        = true;
        gameplay::collect_detonation(view, explosions, spec, random, detonation);
        // Players: the packet with their own knockback, and the damage.
        net::Explosion packet;
        packet.x      = centre.x;
        packet.y      = centre.y;
        packet.z      = centre.z;
        packet.radius = spec.power;
        for (const BlockPos pos : detonation.blocks) {
            (void)packet.add_record(pos);
        }
        for (const BlockPos pos : detonation.air) {
            (void)packet.add_record(pos);
        }
        for (const NetherPlayer& player : players) {
            if (dist_sq(player.feet, centre) >= 64.0 * 64.0) {
                continue;
            }
            const gameplay::ExplosionHit hit = explosions.hit_entity(
                view, spec, gameplay::player_box(player.feet), player.feet, player.feet.y + 1.62);
            packet.knockback_x = hit.touched ? static_cast<f32>(hit.impulse.x) : 0.0F;
            packet.knockback_y = hit.touched ? static_cast<f32>(hit.impulse.y) : 0.0F;
            packet.knockback_z = hit.touched ? static_cast<f32>(hit.impulse.z) : 0.0F;
            if (host->send_to) {
                host->send_to(player.entity_id, net::clientbound::kExplosion,
                              net::encode_explosion(packet));
            }
            if (hit.touched && !player.creative && host->hurt_player) {
                (void)host->hurt_player(player.entity_id, hit.damage,
                                        gameplay::DamageKind::Fireball);
            }
        }
        (void)source;
        gameplay::destroy_detonation(*host->level, explosions, block_loot, spec, tnt_block, random,
                                     loot_random, detonation);
        for (const gameplay::Detonation::Dropped& dropped : detonation.drops) {
            if (host->drop_item) {
                host->drop_item(Vec3d{static_cast<f64>(dropped.pos.x) + 0.5,
                                      static_cast<f64>(dropped.pos.y) + 0.5,
                                      static_cast<f64>(dropped.pos.z) + 0.5},
                                net::ItemStack{dropped.drop.item,
                                               static_cast<i8>(std::min(dropped.drop.count, 64)),
                                               {}});
            }
        }
        // A charge that leaves fire: one crater cell in three, where the cell
        // is empty and stands on something solid (explosion.hpp names the odds).
        for (const BlockPos pos : detonation.blocks) {
            if (explosions.rolls_fire(spec, random)) {
                const registry::BlockStateId below = view.block_at(pos.below());
                if (blocks.face_is_sturdy(below, registry::BlockRegistry::Face::Up)) {
                    place_fire(pos);
                }
            }
        }
        ++stats.blasts;
    }

    void fly_fireballs() {
        for (usize i = fireballs.size(); i-- > 0;) {
            Fireball&            ball  = fireballs[i];
            entity::EntityState* state = world.mutable_state(ball.handle);
            if (state == nullptr || state->removed) {
                fireballs.erase(fireballs.begin() + static_cast<isize>(i));
                continue;
            }
            ++ball.age;
            const Vec3d before = state->position;
            Vec3d       pos    = state->position;
            Vec3d       vel    = state->velocity;
            gameplay::step_fireball(pos, vel, ball.power, false);
            // What it meets on the way: a player's box, then a block. Four
            // samples along the step, the first contact wins.
            bool           done = false;
            constexpr i32  kSamples = 4;
            const f64      half = static_cast<f64>(state->width) * 0.5;
            for (i32 s = 1; s <= kSamples && !done; ++s) {
                const f64   t = static_cast<f64>(s) / kSamples;
                const Vec3d at{before.x + (pos.x - before.x) * t, before.y + (pos.y - before.y) * t,
                               before.z + (pos.z - before.z) * t};
                const AABB box = AABB::from_entity(at, static_cast<f64>(state->width),
                                                   static_cast<f64>(state->height));
                for (const NetherPlayer& player : players) {
                    if (!player.alive || !gameplay::player_box(player.feet).intersects(box)) {
                        continue;
                    }
                    ++stats.hits;
                    if (ball.large) {
                        if (host->hurt_player) {
                            (void)host->hurt_player(player.entity_id, gameplay::kLargeFireballDamage,
                                                    gameplay::DamageKind::Fireball);
                        }
                        explode(at, ball.owner);
                    } else if (host->hurt_player) {
                        (void)host->hurt_player(player.entity_id, gameplay::kSmallFireballDamage,
                                                gameplay::DamageKind::Fireball);
                    }
                    done = true;
                    break;
                }
                if (done) {
                    break;
                }
                if (collisions.overlaps(box)) {
                    if (ball.large) {
                        explode(at, ball.owner);
                    } else {
                        // Fire in the last free cell before the block.
                        const f64 back = static_cast<f64>(s - 1) / kSamples;
                        const Vec3d prev{before.x + (pos.x - before.x) * back,
                                         before.y + (pos.y - before.y) * back + half,
                                         before.z + (pos.z - before.z) * back};
                        place_fire(BlockPos{static_cast<i32>(std::floor(prev.x)),
                                            static_cast<i32>(std::floor(prev.y)),
                                            static_cast<i32>(std::floor(prev.z))});
                    }
                    done = true;
                }
            }
            if (done || ball.age > 600 || !view.is_loaded(BlockPos{static_cast<i32>(pos.x),
                                                                   static_cast<i32>(pos.y),
                                                                   static_cast<i32>(pos.z)})) {
                state->removed = true;
                fireballs.erase(fireballs.begin() + static_cast<isize>(i));
                continue;
            }
            state->position = pos;
            state->velocity = vel;
        }
    }

    // ── Bartering ───────────────────────────────────────────────────────────

    [[nodiscard]] net::ItemStack stack_of(const gameplay::BarterDrop& drop) const {
        std::string_view item = drop.item;
        const bool       book = item == "minecraft:book" && drop.soul_speed_level > 0;
        if (book) {
            item = "minecraft:enchanted_book";
        }
        net::ItemStack stack{item_id(item), static_cast<i8>(std::clamp(drop.count, 1, 64)), {}};
        if (!drop.potion.empty() || drop.soul_speed_level > 0) {
            nbt::Document document;
            document.root = nbt::Tag::make_compound();
            if (!drop.potion.empty()) {
                (void)document.root.put("Potion", nbt::Tag{std::string{drop.potion}});
            }
            if (drop.soul_speed_level > 0) {
                gameplay::EnchantmentList list;
                list.set(gameplay::Enchantment::SoulSpeed, drop.soul_speed_level);
                gameplay::write_enchantments(document.root, list, book);
            }
            stack.nbt = nbt::write(document);
        }
        return stack;
    }

    void barter_tick() {
        {
            const std::scoped_lock lock{queue_mutex};
            interacts_now.swap(interacts);
            interacts.clear();
        }
        for (const auto& [player, id] : interacts_now) {
            const entity::EntityHandle handle = world.find(id);
            const entity::EntityState* state  = world.state(handle);
            if (state == nullptr || state->removed || state->type != piglin_type ||
                admiring.contains(id) || anger.contains(id) || barter.empty()) {
                continue;
            }
            if (host == nullptr || !host->take_held ||
                !host->take_held(player, "minecraft:gold_ingot")) {
                continue;
            }
            admiring[id] = Admiring{now + gameplay::kAdmireTicks, player};
            send_offhand(id, true);
        }
        interacts_now.clear();
        for (auto it = admiring.begin(); it != admiring.end();) {
            if (it->second.until > now) {
                ++it;
                continue;
            }
            const entity::EntityHandle handle = world.find(it->first);
            if (const entity::EntityState* state = world.state(handle);
                state != nullptr && !state->removed && host != nullptr && host->drop_item) {
                const gameplay::BarterDrop drop = barter.draw(random);
                host->drop_item(Vec3d{state->position.x, state->position.y + 1.0, state->position.z},
                                stack_of(drop));
                ++stats.barters;
                OV_LOG_DEBUG("a piglin bartered {} x{}", drop.item, drop.count);
            }
            send_offhand(it->first, false);
            it = admiring.erase(it);
        }
    }

    // ── Anger ───────────────────────────────────────────────────────────────

    [[nodiscard]] entity::EntityHandle proxy_of(i32 player) const {
        const auto found = std::ranges::find(proxies, player, &Proxy::player);
        return found == proxies.end() ? entity::kNoEntity : found->any;
    }

    void enrage(entity::EntityHandle handle, entity::EntityHandle proxy) {
        const entity::EntityState* state = world.state(handle);
        auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle));
        if (state == nullptr || mob == nullptr || proxy == entity::kNoEntity) {
            return;
        }
        // 20 to 39 seconds (minecraft.wiki "Zombified Piglin").
        anger[state->network_id]    = Anger{now + 400 + random.next_int(380), proxy};
        mob->mutable_brain().target = proxy;
    }

    void calm_down() {
        for (auto it = anger.begin(); it != anger.end();) {
            if (it->second.until > now) {
                ++it;
                continue;
            }
            const entity::EntityHandle handle = world.find(it->first);
            if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle))) {
                if (mob->mutable_brain().target == it->second.proxy) {
                    mob->mutable_brain().target = entity::kNoEntity;
                }
            }
            it = anger.erase(it);
        }
    }

    // ── Striders ────────────────────────────────────────────────────────────

    void striders() {
        for (auto& [id, cold] : shaking) {
            entity::EntityState* state = world.mutable_state(world.find(id));
            if (state == nullptr || state->removed) {
                continue;
            }
            const BlockPos feet{static_cast<i32>(std::floor(state->position.x)),
                                static_cast<i32>(std::floor(state->position.y)),
                                static_cast<i32>(std::floor(state->position.z))};
            const auto lava = [&](BlockPos at) {
                return blocks.block_name(blocks.block_of(view.block_at(at))) == "minecraft:lava";
            };
            const bool in_lava = lava(feet) || lava(feet.below());
            // It stands on lava: a strider that is in it rises to its surface.
            if (lava(feet)) {
                BlockPos top = feet;
                while (lava(top) && top.y < 255) {
                    top = top.above();
                }
                state->position.y = static_cast<f64>(top.y);
                state->velocity.y = 0.0;
                state->on_ground  = true;
            }
            const bool now_cold = gameplay::strider_cold(in_lava);
            if (now_cold != cold) {
                cold = now_cold;
                // The pace changes with it: a fresh brain at the other kind's
                // speeds (goals hold their speed from when they were built).
                const gameplay::MobKind* kind =
                    cold ? &gameplay::strider_cold_kind() : gameplay::mob_kind("minecraft:strider");
                if (kind != nullptr) {
                    world.set_logic(world.find(id),
                                    std::make_unique<gameplay::Mob>(*kind, state->width,
                                                                    state->height, id,
                                                                    gameplay::kNoQuarry));
                }
                net::MetadataWriter fields;
                fields.boolean_value(kStriderShaking, cold);
                deliver_all(net::clientbound::kEntityMetadata,
                            net::encode_entity_metadata(id, fields.take()));
            }
        }
    }

    // ── The spawner ─────────────────────────────────────────────────────────

    void spawn_tick() {
        spawn_players.clear();
        for (const NetherPlayer& player : players) {
            if (player.alive) {
                spawn_players.push_back(player.feet);
            }
        }
        if (spawn_players.empty()) {
            return;
        }
        if (now % 20 == 0 || spawn_ticking.empty()) {
            spawn_ticking.clear();
            for (const Vec3d& who : spawn_players) {
                const i32 cx = static_cast<i32>(std::floor(who.x)) >> 4;
                const i32 cz = static_cast<i32>(std::floor(who.z)) >> 4;
                for (i32 dz = -8; dz <= 8; ++dz) {
                    for (i32 dx = -8; dx <= 8; ++dx) {
                        const ChunkPos pos{cx + dx, cz + dz};
                        if (host->ticking && host->ticking(pos) &&
                            std::ranges::find(spawn_ticking, pos) == spawn_ticking.end()) {
                            spawn_ticking.push_back(pos);
                        }
                    }
                }
            }
            std::ranges::sort(spawn_ticking, [](ChunkPos a, ChunkPos b) {
                return a.z != b.z ? a.z < b.z : a.x < b.x;
            });
        }
        live.fill(0);
        for (const entity::EntityHandle handle : world.handles()) {
            const entity::EntityState* state = world.state(handle);
            if (state == nullptr || is_proxy(handle) || is_fireball(state->type)) {
                continue;
            }
            ++live[static_cast<usize>(gameplay::category_of(name_of(state->type)))];
        }
        gameplay::SpawnEnvironment environment;
        environment.level             = &view;
        environment.light             = &light;
        environment.players           = spawn_players;
        environment.ticking_chunks    = spawn_ticking;
        environment.live_per_category = live;
        environment.registries        = &registries;
        environment.game_time         = now;
        environment.biomes            = &biomes;
        environment.day_time          = 18000;  // the Nether's fixed time
        environment.nether            = true;
        environment.spawn_top         = 129;
        spawn_requests.clear();
        spawner.spawn_tick(environment, spawn_requests);
        for (const gameplay::SpawnRequest& request : spawn_requests) {
            (void)spawn(request.type_name, request.position);
        }
    }

    void broadcast_moves() {
        for (const i32 gone : world.removed_ids()) {
            deliver_all(net::clientbound::kRemoveEntities, net::encode_remove_entity(gone));
            if (combat != nullptr) {
                combat->forget(gone);
            }
            volleys.erase(gone);
            charges.erase(gone);
            shaking.erase(gone);
            admiring.erase(gone);
            anger.erase(gone);
            main_hand.erase(gone);
        }
        for (const entity::EntityHandle handle : world.handles()) {
            entity::EntityState* state = world.mutable_state(handle);
            if (state == nullptr || is_proxy(handle)) {
                continue;
            }
            if (!state->broadcast_valid) {
                state->broadcast_position = state->position;
                state->broadcast_valid    = true;
            }
            const f64     dx           = state->position.x - state->broadcast_position.x;
            const f64     dy           = state->position.y - state->broadcast_position.y;
            const f64     dz           = state->position.z - state->broadcast_position.z;
            constexpr f64 kHalfQuantum = 0.5 / 4096.0;
            if (std::abs(dx) < kHalfQuantum && std::abs(dy) < kHalfQuantum &&
                std::abs(dz) < kHalfQuantum) {
                continue;
            }
            if (net::fits_in_delta(dx, dy, dz)) {
                deliver_all(net::clientbound::kEntityPosition,
                            net::encode_entity_position(state->network_id, dx, dy, dz,
                                                        state->on_ground));
                state->broadcast_position.x += net::quantised_delta(dx);
                state->broadcast_position.y += net::quantised_delta(dy);
                state->broadcast_position.z += net::quantised_delta(dz);
            } else {
                deliver_all(net::clientbound::kEntityTeleport,
                            net::encode_entity_teleport(state->network_id, state->position.x,
                                                        state->position.y, state->position.z,
                                                        state->yaw, state->pitch, state->on_ground));
                state->broadcast_position = state->position;
            }
        }
    }
};

// ── The table, from the data generator ──────────────────────────────────────

namespace {

[[nodiscard]] std::optional<gameplay::BarterTable> load_barter(const std::filesystem::path& root) {
    const auto path = root / "data" / "minecraft" / "loot_tables" / "gameplay" /
                      "piglin_bartering.json";
    simdjson::dom::parser parser;
    const auto            document = parser.load(path.string());
    if (document.error() != simdjson::SUCCESS) {
        OV_LOG_WARN("nether: no {} — piglins do not barter", path.string());
        return std::nullopt;
    }
    simdjson::dom::array pools;
    if (document.value_unsafe()["pools"].get(pools) != simdjson::SUCCESS || pools.size() != 1) {
        OV_LOG_ERROR("nether: {} is not one pool — refused", path.string());
        return std::nullopt;
    }
    const simdjson::dom::element pool = *pools.begin();
    f64                          rolls = 0.0;
    if (pool["rolls"].get(rolls) != simdjson::SUCCESS || rolls != 1.0) {
        OV_LOG_ERROR("nether: bartering is not one roll — refused");
        return std::nullopt;
    }
    std::vector<gameplay::BarterEntry> entries;
    simdjson::dom::array               list;
    if (pool["entries"].get(list) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    for (const simdjson::dom::element entry : list) {
        gameplay::BarterEntry out;
        std::string_view      name;
        std::string_view      type;
        if (entry["type"].get(type) != simdjson::SUCCESS || type != "minecraft:item" ||
            entry["name"].get(name) != simdjson::SUCCESS) {
            OV_LOG_ERROR("nether: a barter entry of type {} — refused", type);
            return std::nullopt;
        }
        out.item = std::string{name};
        i64 weight = 1;
        (void)entry["weight"].get(weight);
        out.weight = static_cast<i32>(weight);
        simdjson::dom::array functions;
        if (entry["functions"].get(functions) == simdjson::SUCCESS) {
            for (const simdjson::dom::element function : functions) {
                std::string_view kind;
                (void)function["function"].get(kind);
                if (kind == "minecraft:set_count") {
                    simdjson::dom::element count = function["count"];
                    f64                    fixed = 0.0;
                    if (count.get(fixed) == simdjson::SUCCESS) {
                        out.min_count = out.max_count = static_cast<i32>(std::floor(fixed));
                    } else {
                        f64 lo = 1.0;
                        f64 hi = 1.0;
                        (void)count["min"].get(lo);
                        (void)count["max"].get(hi);
                        out.min_count = static_cast<i32>(std::floor(lo));
                        out.max_count = static_cast<i32>(std::floor(hi));
                    }
                } else if (kind == "minecraft:set_potion") {
                    std::string_view id;
                    (void)function["id"].get(id);
                    out.potion = std::string{id};
                } else if (kind == "minecraft:enchant_randomly") {
                    simdjson::dom::array which;
                    std::string_view     only;
                    if (function["enchantments"].get(which) != simdjson::SUCCESS ||
                        which.size() != 1 || (*which.begin()).get(only) != simdjson::SUCCESS ||
                        only != "minecraft:soul_speed") {
                        OV_LOG_ERROR("nether: an enchant_randomly other than Soul Speed — refused");
                        return std::nullopt;
                    }
                    out.soul_speed = true;
                } else {
                    OV_LOG_ERROR("nether: barter function {} is not modelled — refused", kind);
                    return std::nullopt;
                }
            }
        }
        entries.push_back(std::move(out));
    }
    return gameplay::BarterTable{std::move(entries)};
}

}  // namespace

// ── The session ─────────────────────────────────────────────────────────────

NetherMobs::NetherMobs(const registry::Registries& registries, const registry::BlockRegistry& blocks,
                       MobCombat* combat, const gameplay::LootTables* block_loot,
                       std::span<const std::string_view> biome_names,
                       const std::filesystem::path& generated_root, i64 world_seed)
    : impl_(std::make_unique<Impl>(registries, blocks, combat, block_loot, world_seed)) {
    Impl& impl           = *impl_;
    impl.entity_registry = registries.find("minecraft:entity_type");
    impl.item_registry   = registries.find("minecraft:item");
    impl.player_type          = impl.type_id("minecraft:player");
    impl.villager_type        = impl.type_id("minecraft:villager");
    impl.small_fireball_type  = impl.type_id("minecraft:small_fireball");
    impl.fireball_type        = impl.type_id("minecraft:fireball");
    impl.piglin_type          = impl.type_id("minecraft:piglin");
    impl.brute_type           = impl.type_id("minecraft:piglin_brute");
    impl.zombified_type       = impl.type_id("minecraft:zombified_piglin");
    impl.blaze_type           = impl.type_id("minecraft:blaze");
    impl.ghast_type           = impl.type_id("minecraft:ghast");
    impl.strider_type         = impl.type_id("minecraft:strider");
    impl.wither_skeleton_type = impl.type_id("minecraft:wither_skeleton");
    impl.magma_type           = impl.type_id("minecraft:magma_cube");
    if (const auto tnt = blocks.find_block("minecraft:tnt")) {
        impl.tnt_block = *tnt;
    }
    if (const auto fire = blocks.find_block("minecraft:fire")) {
        impl.fire_state = blocks.default_state(*fire);
    }
    if (const auto soul = blocks.find_block("minecraft:soul_fire")) {
        impl.soul_fire_state = blocks.default_state(*soul);
    }
    if (auto table = load_barter(generated_root)) {
        impl.barter = std::move(*table);
    }
    spawning_ready_ =
        load_all_biome_spawners(generated_root, biome_names, impl.spawner, impl.spawner_names) > 0;
    OV_LOG_INFO("nether mobs: spawning {}, bartering over {} entries (total weight {})",
                spawning_ready_ ? "from the Nether's biome lists" : "off",
                impl.barter.entries().size(), impl.barter.total_weight());
}

NetherMobs::~NetherMobs() = default;

NetherMobStats NetherMobs::tick(i64 tick, const NetherMobHost& host) {
    Impl& impl = *impl_;
    impl.host  = &host;
    impl.view.attach(&host);
    // The session's clock is the ticks it has run, not the server's clock:
    // `TickClock` swallows the ticks a slow server misses, and a piglin
    // admires for 120 of *its* ticks. Measured before this: 65 to 74 ticks of
    // the world's age on a Debug server that was falling behind.
    impl.now   = ++impl.ticks_run;
    impl.stats = NetherMobStats{};
    impl.players.clear();
    if (host.players) {
        host.players(impl.players);
    }

    // The children of a magma cube killed last tick.
    const gameplay::MobKind* magma_kind = gameplay::mob_kind("minecraft:magma_cube");
    for (const gameplay::SlimeChild& child : impl.births) {
        if (const auto born = impl.world.spawn("minecraft:magma_cube", child.offset, net::Uuid{});
            born && magma_kind != nullptr) {
            entity::EntityState* state = impl.world.mutable_state(*born);
            state->uuid                = uuid_for(state->network_id);
            state->broadcast_position  = state->position;
            state->broadcast_valid     = true;
            impl.magma.set_size(*state, child.size);
            impl.world.set_logic(*born, std::make_unique<gameplay::Mob>(
                                            *magma_kind, state->width, state->height,
                                            state->network_id, impl.player_type));
            impl.announce(*state);
        }
    }
    impl.births.clear();

    if (spawning_ready_) {
        impl.spawn_tick();
    }
    impl.update_proxies();
    impl.barter_tick();
    impl.calm_down();
    impl.behave();

    gameplay::MobContext context{&impl.collisions, &impl.view, false};
    impl.world.tick(entity::TickContext{tick, &context});

    impl.fly_fireballs();
    impl.striders();
    impl.broadcast_moves();

    usize alive = 0;
    for (const entity::EntityHandle handle : impl.world.handles()) {
        const entity::EntityState* state = impl.world.state(handle);
        if (state != nullptr && !impl.is_proxy(handle) && !impl.is_fireball(state->type)) {
            ++alive;
        }
    }
    impl.stats.alive = alive;
    impl.host        = nullptr;
    impl.view.attach(nullptr);
    return impl.stats;
}

void NetherMobs::send_all(
    const std::function<void(i32 id, std::span<const u8> payload)>& deliver) const {
    const Impl& impl = *impl_;
    for (const entity::EntityHandle handle : impl.world.handles()) {
        const entity::EntityState* state = impl.world.state(handle);
        if (state == nullptr || state->removed || impl.is_proxy(handle)) {
            continue;
        }
        impl.packets(*state, deliver);
    }
}

bool NetherMobs::hurt(i32 network_id, f32 damage, i32 attacker, u8 looting,
                      const NetherMobHost& host) {
    Impl& impl = *impl_;
    if (!owns(network_id)) {
        return false;
    }
    const entity::EntityHandle handle = impl.world.find(network_id);
    entity::EntityState*       state  = impl.world.mutable_state(handle);
    if (state == nullptr || state->removed || impl.is_proxy(handle)) {
        return false;
    }
    if (impl.is_fireball(state->type)) {
        // Punching a fireball back is named, not modelled: it is swung through.
        return true;
    }
    if (impl.combat == nullptr) {
        return false;
    }
    impl.host = &host;
    const MobHurt result = impl.combat->hurt(*state, damage, impl.damage_constants);
    if (!result.applied) {
        impl.host = nullptr;
        return true;
    }
    if (auto* mob = dynamic_cast<gameplay::Mob*>(impl.world.logic(handle))) {
        mob->frighten(100);
    }
    // Anger: a zombified piglin calls every other within 35 blocks across
    // and 10 up and down (minecraft.wiki); a piglin turns on whoever hit it.
    const entity::EntityHandle proxy = impl.proxy_of(attacker);
    if (state->type == impl.zombified_type) {
        const Vec3d centre = state->position;
        for (const entity::EntityHandle other : impl.world.handles()) {
            const entity::EntityState* peer = impl.world.state(other);
            if (peer == nullptr || peer->removed || peer->type != impl.zombified_type) {
                continue;
            }
            if (std::abs(peer->position.x - centre.x) <= 35.0 &&
                std::abs(peer->position.y - centre.y) <= 10.0 &&
                std::abs(peer->position.z - centre.z) <= 35.0) {
                impl.enrage(other, proxy);
            }
        }
    } else if (state->type == impl.piglin_type || state->type == impl.brute_type) {
        impl.enrage(handle, proxy);
    }

    impl.deliver_all(net::clientbound::kDamageEvent,
                     net::encode_damage_event(state->network_id,
                                              damage_type_id(gameplay::DamageKind::PlayerAttack),
                                              attacker, attacker));
    net::MetadataWriter fields;
    fields.float_value(net::metadata::kHealth, state->health);
    impl.deliver_all(net::clientbound::kEntityMetadata,
                     net::encode_entity_metadata(state->network_id, fields.take()));
    if (result.killed) {
        impl.deliver_all(net::clientbound::kEntityEvent,
                         net::encode_entity_event(state->network_id, 3));
        std::vector<gameplay::Drop> drops;
        (void)impl.combat->loot(*state, true, looting, impl.loot_random, drops);
        for (const gameplay::Drop& drop : drops) {
            if (host.drop_item) {
                host.drop_item(Vec3d{state->position.x,
                                     state->position.y + static_cast<f64>(state->height) * 0.5,
                                     state->position.z},
                               net::ItemStack{drop.item, static_cast<i8>(std::min(drop.count, 64)),
                                              {}});
            }
        }
        if (state->type == impl.magma_type) {
            impl.magma.on_death(*state, impl.random, impl.birth_scratch);
            impl.births.insert(impl.births.end(), impl.birth_scratch.begin(),
                               impl.birth_scratch.end());
        }
        state->removed = true;
        impl.combat->forget(state->network_id);
        OV_LOG_INFO("a player killed a Nether {} ({} stacks)", impl.name_of(state->type),
                    drops.size());
    }
    impl.host = nullptr;
    return true;
}

void NetherMobs::queue_interact(i32 player, i32 network_id) {
    if (!owns(network_id)) {
        return;
    }
    const std::scoped_lock lock{impl_->queue_mutex};
    impl_->interacts.emplace_back(player, network_id);
}

std::optional<i32> NetherMobs::summon(std::string_view type, Vec3d at, const NetherMobHost& host) {
    Impl& impl = *impl_;
    impl.host  = &host;
    impl.view.attach(&host);
    const auto made = impl.spawn(type, at);
    impl.host       = nullptr;
    impl.view.attach(nullptr);
    if (!made) {
        return std::nullopt;
    }
    return impl.world.state(*made)->network_id;
}

std::string_view NetherMobs::type_of(i32 network_id) const {
    const entity::EntityState* state = impl_->world.state(impl_->world.find(network_id));
    return state == nullptr ? std::string_view{} : impl_->name_of(state->type);
}

usize NetherMobs::size() const noexcept {
    return impl_->world.size();
}

// ── persistence ─────────────────────────────────────────────────────────────

entity::EntityWorld& NetherMobs::world() noexcept {
    return impl_->world;
}

void NetherMobs::forget(std::span<const i32> ids) {
    Impl& impl = *impl_;
    for (const i32 gone : ids) {
        if (impl.combat != nullptr) {
            impl.combat->forget(gone);
        }
        impl.volleys.erase(gone);
        impl.charges.erase(gone);
        impl.shaking.erase(gone);
        impl.admiring.erase(gone);
        impl.anger.erase(gone);
        impl.main_hand.erase(gone);
    }
}

EntityStorageHost NetherMobs::storage_host(const NetherMobHost& host) {
    Impl*             impl = impl_.get();
    EntityStorageHost out;
    out.attach = [impl](entity::EntityHandle handle, std::string_view type) {
        impl->behaviour(handle, type);
    };
    out.announce = [impl, &host](const entity::EntityState& state) {
        if (host.broadcast) {
            impl->packets(state, host.broadcast);
        }
    };
    out.ignore = [impl](i32 network_id) {
        const entity::EntityHandle handle = impl->world.find(network_id);
        const entity::EntityState* state  = impl->world.state(handle);
        return state == nullptr || impl->is_proxy(handle) || impl->is_fireball(state->type);
    };
    out.write_extra = [impl](const entity::EntityState& state, nbt::Tag& out_tag) {
        if (state.type == impl->magma_type) {
            (void)out_tag.put("Size", nbt::Tag{std::max(impl->magma.size_of(state.network_id), 1) - 1});
            if (!out_tag.contains("wasOnGround")) {
                (void)out_tag.put("wasOnGround", nbt::Tag::make_bool(state.on_ground));
            }
        }
        // The main hand, as a mob's `HandItems` holds it: [main, off].
        if (const auto hand = impl->main_hand.find(state.network_id);
            hand != impl->main_hand.end() && impl->item_registry) {
            nbt::Tag hands = nbt::Tag::make_list(nbt::TagType::Compound);
            (void)hands.push(item_stack_tag(impl->registries, *impl->item_registry, hand->second)
                                 .value_or(nbt::Tag::make_compound()));
            (void)hands.push(nbt::Tag::make_compound());
            (void)out_tag.put("HandItems", std::move(hands));
        }
        if (state.type == impl->zombified_type && !out_tag.contains("AngerTime")) {
            (void)out_tag.put("AngerTime", nbt::Tag{i32{0}});
        }
        if (state.type == impl->ghast_type && !out_tag.contains("ExplosionPower")) {
            (void)out_tag.put("ExplosionPower", nbt::Tag{i8{1}});
        }
    };
    out.read_extra = [impl](entity::EntityState& state, const nbt::Tag& compound) {
        if (state.type == impl->magma_type) {
            impl->magma.set_size(state, static_cast<i32>(get_i64(compound, "Size", 0)) + 1);
        }
        const nbt::Tag* hands = compound.find("HandItems");
        if (hands != nullptr && hands->list() != nullptr && !hands->list()->empty() &&
            impl->item_registry) {
            if (const auto held = item_stack_from(impl->registries, *impl->item_registry,
                                                  &hands->list()->front())) {
                impl->main_hand[state.network_id] = *held;
            }
        }
    };
    return out;
}

}  // namespace ov::server
