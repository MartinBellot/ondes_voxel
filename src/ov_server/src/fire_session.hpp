// Fire, as this server carries it out.
//
// The rules are in ov_gameplay (fire.hpp) and know nothing about chunks,
// players or sockets. What is here is the part that does:
//
//   * a block-rule extension, so a fire's own tick and the neighbour
//     notification every write makes reach `FireRules` (world_ticks.hpp);
//   * a random-tick extension, so lava lights fire (agriculture.hpp);
//   * the environment a fire reads — the rain on one block, the humid biomes,
//     the gamerule, the difficulty — over callbacks into the server's chunks;
//   * burning mobs and players: the counter, the damage, the flag that makes a
//     client draw the flames, and the zombie that catches fire at noon.
//
// Callbacks rather than a reference to the server, as for tnt_gravity.hpp.
//
// ── For the agents writing lightning and portals ────────────────────────────
//
// `ignite(level, pos)` is the entry point for anything that sets fire to the
// world from outside the rules — lightning, a fire charge. It writes a fire
// only where one may stand, through the level, so the notification that
// follows schedules its first tick.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Everything runs on the tick thread with `chunk_mutex` held, except
// `set_mob_on_fire`, which a player's Fire Aspect calls from the network
// thread: it only appends to a list behind its own lock.
#pragma once

#include "agriculture.hpp"
#include "mob_combat.hpp"
#include "tnt_gravity.hpp"
#include "world_ticks.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/fire.hpp"
#include "ov/protocol/play.hpp"

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// The world state a fire reads, refreshed by the server once a tick.
struct FireWorld {
    bool fire_tick{true};
    /// `Level.isRaining`: the rain level above 0.2.
    bool raining{false};
    i32  difficulty{2};
    u8   sky_darken{0};
};

/// What the module reads from the chunks. Every callback is called with the
/// chunk lock held, from the tick thread.
struct FireHost {
    std::function<registry::BlockStateId(BlockPos)> block_at;
    /// The biome index at a position, -1 when the chunk is not resident.
    std::function<i32(BlockPos)> biome_at;
    /// The first free y above the column's MOTION_BLOCKING surface — rain
    /// reaches a block at or above it.
    std::function<i32(i32 x, i32 z)> rain_top;
    std::function<u8(BlockPos)> sky_light;
    std::function<u8(BlockPos)> block_light;
    /// A TNT block was burnt away here: prime it.
    std::function<void(BlockPos)> prime_tnt;
};

/// What the mob pass reaches outside the module for. Called inside the entity
/// pass, which holds both locks.
struct FireMobHost {
    Deliver                                           broadcast;
    std::function<void(Vec3d, const net::ItemStack&)> drop_item;
};

/// What the player pass reaches for.
struct FirePlayerIo {
    /// Attempt one hit. True when it landed.
    std::function<bool(gameplay::DamageKind, f32)> hurt;
    /// The shared-flags byte changed its burning bit: send index 0.
    std::function<void(bool on_fire)> flag;
};

struct FireMobStats {
    usize burning{0};
    usize hurt{0};
    usize died{0};
    usize sun_lit{0};
};

class FireSession final : public BlockRuleExtension,
                          public RandomTickExtension,
                          public gameplay::FireEnvironment {
public:
    FireSession(const registry::BlockRegistry& blocks, const registry::Registries& registries,
                FireHost host, MobCombat* mob_combat);

    [[nodiscard]] const gameplay::FireRules& rules() const noexcept { return rules_; }

    void set_world(const FireWorld& world) noexcept { world_ = world; }

    // ── BlockRuleExtension ──────────────────────────────────────────────────
    void neighbour_changed(ServerLevel& level, BlockPos pos) override;
    bool scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) override;

    // ── RandomTickExtension ─────────────────────────────────────────────────
    [[nodiscard]] bool ticks_randomly(registry::BlockStateId state) const noexcept override;
    void random_tick(world::LevelWriter& level, BlockPos pos, registry::BlockStateId state) override;

    // ── FireEnvironment ─────────────────────────────────────────────────────
    [[nodiscard]] bool fire_tick() const override { return world_.fire_tick; }
    [[nodiscard]] bool is_raining() const override { return world_.raining; }
    [[nodiscard]] bool is_raining_at(BlockPos pos) const override;
    [[nodiscard]] bool increased_burnout(BlockPos pos) const override;
    [[nodiscard]] i32  difficulty() const override { return world_.difficulty; }
    void               prime_tnt(BlockPos pos) override;

    /// Light a fire here if one may stand: lightning, a fire charge. Writes
    /// through `level`; the caller settles.
    bool ignite(ServerLevel& level, BlockPos pos);

    // ── Things on fire ──────────────────────────────────────────────────────

    /// What a box at `feet`, `width` wide and `height` tall is touching.
    [[nodiscard]] gameplay::FireContact contact(Vec3d feet, f64 width, f64 height) const;

    /// Set a mob alight for `seconds` — Fire Aspect, a Flame arrow. Applied at
    /// the next mob pass. Thread-safe.
    void set_mob_on_fire(i32 network_id, i32 seconds);

    /// One tick of every living mob's fire, after the entity tick.
    FireMobStats tick_mobs(entity::EntityWorld& world, const FireMobHost& host);

    /// One tick of one player's fire. `mortal` false for creative and
    /// spectator, who neither burn nor are hurt; `resistant` for Fire
    /// Resistance, which stops the damage and not the flames.
    void tick_player(gameplay::EntityFire& fire, bool& flag_sent,
                     const gameplay::FireContact& contact, bool mortal, bool resistant,
                     const FirePlayerIo& io);

    /// The burning mobs still tracked. Instrumentation.
    [[nodiscard]] usize tracked() const noexcept { return mobs_.size(); }

private:
    struct MobFire {
        gameplay::EntityFire fire{};
        bool                 flag_sent{false};
    };

    [[nodiscard]] bool biome_rains(i32 biome, i32 y) const noexcept;
    void hurt_mob(entity::EntityState& state, gameplay::DamageKind kind, f32 amount,
                  bool burning, const FireMobHost& host, FireMobStats& stats);

    const registry::BlockRegistry* blocks_;
    gameplay::FireRules            rules_;
    FireHost                       host_;
    MobCombat*                     mob_combat_;
    FireWorld                      world_{};

    gameplay::FireRandom          random_{0x4649'5245'0000'0001LL};
    math::XoroshiroRandomSource   loot_random_{0x4649'5245'4C4F'4F54LL};
    gameplay::DamageConstants     damage_constants_{};
    gameplay::EntityFireConstants constants_{};

    /// The humid biomes, by index (`#minecraft:increased_fire_burnout`).
    std::vector<u8> humid_;

    std::unordered_map<i32, MobFire> mobs_;
    std::vector<i32>                 gone_;

    struct PendingFire {
        i32 network_id{0};
        i32 seconds{0};
    };
    mutable std::mutex       pending_mutex_;
    std::vector<PendingFire> pending_;
    std::vector<PendingFire> applying_;

    i32 zombie_type_{-1};
    i32 skeleton_type_{-1};
};

}  // namespace ov::server
