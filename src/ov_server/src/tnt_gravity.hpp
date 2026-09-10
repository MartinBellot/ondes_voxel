// Primed TNT, creepers and falling blocks, as this server carries them out.
//
// The rules are in ov_gameplay (primed_tnt.hpp, falling_block.hpp,
// explosion.hpp) and know nothing about players, sockets or chunks. What is
// here is the part that does: spawning the entities and telling clients about
// them, handing an explosion the players and the items on the ground, and
// putting what a landing or a crater produces back into the world through the
// same `ServerLevel` every other rule writes through — which is what makes the
// water beside a crater flow in and the sand above it fall.
//
// Callbacks rather than a reference to the server, as for world_ticks.hpp and
// workbench.hpp: this file must not know what a `Player` or a chunk map is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Everything runs on the tick thread except `request_prime`, which a player's
// flint and steel calls from the network thread. That one only appends to a
// list behind its own lock; the entity is spawned by the tick, so the level's
// random source and the entity world are only ever touched by one thread.
#pragma once

#include "mob_combat.hpp"
#include "world_ticks.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/explosion.hpp"
#include "ov/gameplay/falling_block.hpp"
#include "ov/gameplay/loot.hpp"
#include "ov/gameplay/primed_tnt.hpp"
#include "ov/gameplay/redstone.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// Send one packet somewhere.
using Deliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// A connected player, as an explosion sees one.
struct BlastPlayer {
    Vec3d feet{};
    /// Creative players are pushed but not hurt, and no creeper swells at one.
    bool creative{false};
    /// This player's own connection. The Explosion packet is per player: the
    /// knockback in it is theirs.
    Deliver send;
    /// Take explosion damage. Empty when the player cannot be hurt.
    std::function<void(f32)> hurt;
};

/// What the module reaches outside itself for.
struct TntGravityHost {
    /// Every connected player, one at a time.
    std::function<void(const std::function<void(BlastPlayer&)>&)> each_player;
    /// Every connected player gets this.
    Deliver broadcast;
    /// Put a stack on the ground at a point.
    std::function<void(Vec3d, const net::ItemStack&)> drop_item;
    /// Remove every ground item the predicate says an explosion destroyed.
    std::function<void(const std::function<bool(Vec3d)>&)> sweep_items;
    /// The feet of every player a creeper may swell at: connected, in
    /// survival. Fills a vector the module reuses, so a tick with creepers in
    /// it does not build a callback per player.
    std::function<void(std::vector<Vec3d>&)> creeper_targets;
};

/// What one tick did, for the log and for the report.
struct TntGravityStats {
    usize blasts{0};
    usize blocks_destroyed{0};
    usize records_sent{0};
    usize primed{0};
    usize falls{0};
    usize landed{0};
    usize dropped{0};
};

class TntGravity final : public BlockRuleExtension {
public:
    TntGravity(const registry::BlockRegistry& blocks, const registry::Registries& registries,
               const gameplay::LootTables* loot, MobCombat* mob_combat);

    /// The redstone engine whose signals light TNT. Not owned.
    void set_redstone(const gameplay::Redstone* redstone) noexcept { redstone_ = redstone; }

    // ── BlockRuleExtension: called by the world-tick drain ──────────────────

    void neighbour_changed(ServerLevel& level, BlockPos pos) override;
    bool scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) override;

    // ── Entities ────────────────────────────────────────────────────────────

    /// Is this entity type one this module spawns and speaks for?
    [[nodiscard]] bool owns(i32 type) const noexcept {
        return type == tnt_type_ || type == falling_type_;
    }

    /// The packets that make one of this module's entities appear, for a
    /// player joining after it was spawned.
    void spawn_packets(entity::EntityWorld& world, const entity::EntityState& state,
                       const Deliver& deliver) const;

    /// A TNT block became a primed TNT somewhere the tick cannot see — a
    /// player's flint and steel. Spawned on the next tick. Thread-safe.
    void request_prime(BlockPos pos, i32 fuse);

    /// Is anything waiting to be spawned? The server skips its entity pass
    /// when there are no entities, and must not when there are about to be.
    [[nodiscard]] bool has_pending() const;

    /// Spawn what the block drain and the network thread asked for.
    void spawn_pending(entity::EntityWorld& world, const Deliver& deliver);

    /// One tick of every creeper's countdown, against the nearest player.
    void tick_creepers(entity::EntityWorld& world, const world::LevelView& level,
                       const TntGravityHost& host, const Deliver& deliver);

    /// Everything the entity tick asked for: landings, explosions, and the
    /// fuse every live TNT re-sends. Writes through `level` and settles what
    /// it wrote through `ticks`.
    TntGravityStats after_entity_tick(entity::EntityWorld& world, ServerLevel& level,
                                      WorldTicks& ticks, const TntGravityHost& host,
                                      const Deliver& deliver);

    [[nodiscard]] const gameplay::FallingBlocks& falling() const noexcept { return falling_; }
    [[nodiscard]] const gameplay::Explosions&    explosions() const noexcept {
        return explosions_;
    }

private:
    void spawn_primed(entity::EntityWorld& world, BlockPos pos, i32 fuse, const Deliver& deliver);
    void spawn_falling(entity::EntityWorld& world, const gameplay::FallStart& start,
                       const Deliver& deliver);
    void detonate(entity::EntityWorld& world, ServerLevel& level,
                  const gameplay::BlastEvents::Blast& blast, const TntGravityHost& host,
                  const Deliver& deliver, TntGravityStats& stats);
    void drop_block_item(registry::BlockStateId state, BlockPos cell,
                         const TntGravityHost& host) const;

    const registry::BlockRegistry* blocks_;
    const registry::Registries*    registries_;
    const gameplay::LootTables*    loot_;
    MobCombat*                     mob_combat_;
    const gameplay::Redstone*      redstone_{nullptr};

    gameplay::Explosions    explosions_;
    gameplay::FallingBlocks falling_;
    gameplay::FallingEvents falling_events_;
    gameplay::BlastEvents   blast_events_;
    gameplay::Detonation    detonation_;
    gameplay::DamageConstants damage_constants_{};

    /// The level's own random source, which in 1.20.1 is `java.util.Random`:
    /// the rays, the prime's direction and the chained fuses all draw from it.
    /// Fixed seed — two runs of the same server must make the same craters.
    math::LegacyRandomSource    random_{0x54'4E'54'00'47'52'41'56LL};
    math::XoroshiroRandomSource loot_random_{0x0B1A57ULL, 0xD5A11ED5ULL};

    i32               tnt_type_{-1};
    i32               falling_type_{-1};
    i32               creeper_type_{-1};
    i32               item_registry_valid_{0};
    registry::RegistryId item_registry_{};
    registry::BlockId tnt_block_{0};

    mutable std::mutex                 pending_mutex_;
    std::vector<gameplay::FallStart>   pending_falls_;
    struct PendingPrime {
        BlockPos pos{};
        i32      fuse{0};
    };
    std::vector<PendingPrime> pending_primes_;
    /// Swapped out under the lock, so spawning happens outside it.
    std::vector<gameplay::FallStart> spawning_falls_;
    std::vector<PendingPrime>        spawning_primes_;

    /// Each creeper's countdown, by wire id.
    std::unordered_map<i32, gameplay::CreeperSwell> creepers_;
    std::vector<Vec3d>                              targets_;

    /// A dropped item's eye height, from the measured entity table. The point
    /// an explosion pushes it from.
    f64 item_eye_{0.2125};
};

}  // namespace ov::server
