// ── nether-2 ── The Nether's mobs, as this server carries them out.
//
// The Nether had terrain, portals and players, and no living thing: the
// server's `EntityWorld` is the overworld's, and every system that touches it —
// spawning, drowning, husbandry, projectiles, the broadcast of every move — was
// written for that one level. Rather than teach each of them a second
// dimension, the Nether gets a world of its own here, with the parts of those
// systems its mobs use: natural spawning by the Nether's biome lists and rules
// (spawning.hpp, `SpawnEnvironment::nether`), the same walking brains
// (`gameplay::Mob`), and what only the Nether's mobs do — the piglin's barter,
// the blaze's volley and the ghast's fireball, the strider's shiver, the magma
// cube's sizes, the zombified piglins' shared anger.
//
// Callbacks rather than a reference to the server, as for tnt_gravity.hpp and
// projectiles.hpp: this file does not know what a `Player` or a chunk map is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Every call is made with the server's `players_mutex` held, and `tick` with
// `chunk_mutex` too: the entity world is touched by the tick and by a player's
// hit, and that mutex is what serialises the two, exactly as for the
// overworld's mobs. `queue_interact` alone may be called from the network
// thread without it; the queue has its own lock.
//
// ── Wire ids ────────────────────────────────────────────────────────────────
//
// Three million up (the overworld's mobs start at one million, players at
// one): `owns` answers by range, so a hit or a click is routed without a
// lookup in the wrong world.
#pragma once

#include "mob_combat.hpp"
#include "slimes.hpp"
#include "world_ticks.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/explosion.hpp"
#include "ov/gameplay/loot.hpp"
#include "ov/gameplay/nether_mobs.hpp"
#include "ov/gameplay/primed_tnt.hpp"
#include "ov/gameplay/spawning.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// A player standing in the Nether, as the session sees one.
struct NetherPlayer {
    i32   entity_id{0};
    Vec3d feet{};
    bool  creative{false};
    /// Dead or waiting to respawn: nothing aims at it.
    bool alive{true};
    /// Wears at least one piece of golden armour: piglins leave it alone.
    bool gold_armour{false};
};

/// What the session reaches outside itself for.
struct NetherMobHost {
    /// A state in the Nether: resident chunks only, air elsewhere.
    std::function<registry::BlockStateId(BlockPos)> block_at;
    /// Is the chunk under this position resident?
    std::function<bool(BlockPos)> loaded;
    /// Is this chunk ticking in the Nether's map?
    std::function<bool(ChunkPos)> ticking;
    /// The biome index (the codec's order) at a position.
    std::function<u16(BlockPos)> biome_at;
    /// Block light at a position; the Nether has no sky light.
    std::function<u8(BlockPos)> block_light;
    /// Every player in the Nether. Fills a vector the session reuses.
    std::function<void(std::vector<NetherPlayer>&)> players;
    /// To every player in the Nether.
    std::function<void(i32 id, std::span<const u8> payload)> broadcast;
    /// A stack on the ground, in the Nether.
    std::function<void(Vec3d at, const net::ItemStack& stack)> drop_item;
    /// Hurt a player. True when it landed.
    std::function<bool(i32 player, f32 damage, gameplay::DamageKind kind)> hurt_player;
    /// Take one of `item` out of the player's hand. True when there was one.
    std::function<bool(i32 player, std::string_view item)> take_held;
    /// The Nether's level: the fire a fireball lights and the crater a ghast's
    /// makes are written through it, so its rules (portals, fluids) hear them.
    ServerLevel* level{nullptr};
    /// Explosion packets and knockback for one player near a blast.
    std::function<void(const std::function<void(i32 player, Vec3d feet, bool creative)>&)>
        each_player;
    std::function<void(i32 player, i32 id, std::span<const u8> payload)> send_to;
};

/// What one tick did, for the log and the end-to-end check.
struct NetherMobStats {
    usize spawned{0};
    usize alive{0};
    usize shots{0};
    usize hits{0};
    usize barters{0};
    usize fires{0};
    usize blasts{0};
};

class NetherMobs {
public:
    /// `combat` keeps the damage windows and draws the entity loot, as for the
    /// overworld's mobs; `block_loot` is what a ghast's crater drops (null:
    /// nothing). `biome_names` is the codec's order, which is what a chunk's
    /// biome indices point into.
    NetherMobs(const registry::Registries& registries, const registry::BlockRegistry& blocks,
               MobCombat* combat, const gameplay::LootTables* block_loot,
               std::span<const std::string_view> biome_names,
               const std::filesystem::path& generated_root, i64 world_seed);
    ~NetherMobs();

    NetherMobs(const NetherMobs&)            = delete;
    NetherMobs& operator=(const NetherMobs&) = delete;

    /// Is this wire id one of the Nether's entities?
    [[nodiscard]] static bool owns(i32 network_id) noexcept {
        return network_id >= kFirstId && network_id < kFirstId + 1'000'000;
    }

    /// Spawning, brains, shots, barters, movement — one tick.
    NetherMobStats tick(i64 tick, const NetherMobHost& host);

    /// Every entity, to one player arriving in the Nether.
    void send_all(const std::function<void(i32 id, std::span<const u8> payload)>& deliver) const;

    /// A player's hit on one of ours. False when the id is not a live mob here.
    bool hurt(i32 network_id, f32 damage, i32 attacker, u8 looting, const NetherMobHost& host);

    /// A right click on one of ours (network thread). Acted on by the tick.
    void queue_interact(i32 player, i32 network_id);

    /// Put a mob into the Nether (a command, a test). Its wire id, or nothing
    /// for a type this world cannot hold.
    std::optional<i32> summon(std::string_view type, Vec3d at, const NetherMobHost& host);

    /// The registry name of one of ours, or empty.
    [[nodiscard]] std::string_view type_of(i32 network_id) const;

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] bool  spawning_ready() const noexcept { return spawning_ready_; }

    static constexpr i32 kFirstId = 3'000'000;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool                  spawning_ready_{false};
};

}  // namespace ov::server
