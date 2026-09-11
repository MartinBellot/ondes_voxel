// The weather, the lightning and the beds, as this server carries them out.
//
// The rules are in ov_gameplay (weather.hpp, sleep.hpp) and know nothing about
// players, sockets or chunk maps. What is here is the part that does:
//
//   * the **chunk tick** of precipitation — one chunk in sixteen of those the
//     random tick selected picks a column each tick; water under it freezes in
//     the cold, snow lies on it and cauldrons fill while it rains — and the
//     chunk tick of **lightning**, one chunk in 100 000 per tick under a storm;
//   * the **bolts**: target (a lightning rod within 128 blocks, a living thing
//     that sees the sky, else the surface), the entity the clients are told
//     about, what it hurts and what it converts;
//   * the **beds**: the click, the verdict and its message, lying down, the
//     night skip, waking, and the respawn point a bed sets;
//   * "is it raining here", which farmland asks.
//
// Callbacks rather than a reference to the server, as world_ticks.hpp and
// tnt_gravity.hpp do: this file must not know what a `Player` is.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Everything runs on the tick thread with `players_mutex` and `chunk_mutex`
// held, except `request_bed` and `request_leave`, which the network thread
// calls when a client clicks a bed or asks to get up: they only append to a
// list behind the session's own lock, and the tick answers them.
#pragma once

#include "commands/world_state.hpp"
#include "mob_combat.hpp"
#include "world_ticks.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/sleep.hpp"
#include "ov/gameplay/weather.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/types.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk_map.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// Send one packet somewhere.
using WeatherDeliver = std::function<void(i32 id, std::span<const u8> payload)>;

/// A connected player, as the weather sees one.
struct WeatherPlayer {
    i32       entity_id{0};
    net::Uuid uuid{};
    Vec3d     feet{};
    f32       yaw{0.0F};
    f32       pitch{0.0F};
    bool      creative{false};
    bool      spectator{false};
    bool      alive{true};
};

/// What the session reaches outside itself for.
struct WeatherHost {
    /// Every connected player gets this.
    WeatherDeliver broadcast;
    /// One player gets this.
    std::function<void(i32 player, i32 id, std::span<const u8> payload)> send_to;
    /// Every connected, confirmed player, in a vector the session reuses.
    std::function<void(std::vector<WeatherPlayer>&)> players;
    /// A fresh wire id for a bolt.
    std::function<i32()> allocate_entity_id;
    /// Move a player (lying down, standing up). `sync` also tells their own
    /// client, which the game does when they stand up and not when they lie.
    std::function<void(i32 player, Vec3d feet, f32 yaw, f32 pitch, bool sync)> place_player;
    /// Hurt a player with lightning. Creative players are never passed.
    std::function<void(i32 player, f32 amount)> hurt_player;
    /// A mob becomes another: remove the old one and spawn `type` where it
    /// stood. False when the new type cannot be spawned here.
    std::function<bool(entity::EntityHandle old, std::string_view type)> convert_mob;
    /// A creeper struck by lightning is charged from now on.
    std::function<void(i32 network_id)> charge_creeper;
    /// Items on the ground that a predicate says a bolt burnt.
    std::function<void(const std::function<bool(Vec3d)>&)> sweep_items;
    /// The respawn point a bed sets. True when it changed — the game only says
    /// "respawn point set" then.
    std::function<bool(const net::Uuid& uuid, BlockPos head, f32 angle)> set_spawn;
    /// An explosion where beds do not work. Absent: the explosion is refused
    /// and said.
    std::function<void(Vec3d centre, f32 power, bool fire)> explode;
    /// Put a stack on the ground — the loot of a mob a bolt killed.
    std::function<void(Vec3d, const net::ItemStack&)> drop_item;
    /// ── fire ── Light a fire here if one may stand (fire_session.hpp).
    /// Called with the chunk lock held; the weather tick settles the writes.
    /// Absent: bolts set nothing alight, and that is said once.
    std::function<bool(BlockPos)> ignite;
};

/// What one tick did.
struct WeatherStats {
    gameplay::PrecipitationStats precipitation;
    usize                        columns{0};
    usize                        strikes{0};
    usize                        rods{0};
    usize                        bolts_alive{0};
    usize                        hurt{0};
    usize                        converted{0};
    usize                        slept{0};
    usize                        woke{0};
    bool                         night_skipped{false};
};

class WeatherSession {
public:
    WeatherSession(const registry::BlockRegistry& blocks, const registry::Registries& registries,
                   MobCombat* mob_combat, u64 seed);

    // ── Any thread ──────────────────────────────────────────────────────────

    /// A player clicked a bed at `clicked`. Answered by the next tick.
    void request_bed(i32 player, BlockPos clicked);
    /// A player asked to get up (Player Command, action 2).
    void request_leave(i32 player);

    /// Is this player asleep? The network thread asks, to ignore what a
    /// sleeping client says about where it is.
    [[nodiscard]] bool sleeping(i32 player) const;

    /// Is this state a bed? Read-only, and safe from the network thread.
    [[nodiscard]] bool is_bed(registry::BlockStateId state) const noexcept {
        return beds_.is_bed(state);
    }

    // ── Tick thread ─────────────────────────────────────────────────────────

    /// Precipitation and lightning over the chunks the random tick selected,
    /// then the bolts, then the beds. `world` is changed in place: the night
    /// skip moves its clock and may reset its weather.
    WeatherStats tick(ServerLevel& level, world::ChunkMap& chunks,
                      std::span<const ChunkPos> selected, cmd::WorldState& world,
                      entity::EntityWorld* mobs, const WeatherHost& host);

    /// Is rain falling on this position right now? What farmland asks.
    [[nodiscard]] bool is_raining_at(const world::ChunkMap& chunks, BlockPos pos,
                                     const cmd::Weather& weather) const;

    /// The sky darkening with the weather: 0..11.
    [[nodiscard]] static u8 sky_darken(const cmd::WorldState& world) noexcept;

    /// Where a player whose respawn point is a bed comes back: the stand-up
    /// position beside it, or nothing when the bed is gone (the game then says
    /// `block.minecraft.spawn.not_valid` and uses the world spawn).
    [[nodiscard]] std::optional<Vec3d> bed_respawn(const world::LevelView& level, BlockPos head,
                                                   f32 yaw) const;

    /// Did this player's respawn point come from a bed? A `/spawnpoint` is
    /// "forced" and respawns without one.
    [[nodiscard]] bool spawn_is_bed(const net::Uuid& uuid, BlockPos point) const;

    /// A player left. Forget them, and free their bed.
    void forget(ServerLevel& level, i32 player);

    /// The packets a joining player needs about everyone asleep.
    void join_packets(const WeatherDeliver& deliver) const;

    /// When false, the sleeping count is not announced: a singleplayer world
    /// that is not open to the network says nothing, as the game does.
    void set_announce(bool announce) noexcept { announce_ = announce; }

    /// One chunk in this many is struck per tick of storm. 100 000, the game's;
    /// a test or a measurement lowers it to see bolts at all.
    void set_thunder_chance(i32 chance) noexcept { thunder_chance_ = chance < 1 ? 1 : chance; }

    [[nodiscard]] const gameplay::ClimateNoise& climate() const noexcept { return climate_; }

    /// The metadata indices a sleeping player's pose travels in — measured
    /// (docs/provenance/meteo-sommeil.md): pose at 6 (SLEEPING is 2), the
    /// optional bed position at 14.
    static constexpr u8  kPoseIndex      = 6;
    static constexpr u8  kSleepingPos    = 14;
    static constexpr i32 kPoseStanding   = 0;
    static constexpr i32 kPoseSleeping   = 2;
    static constexpr u8  kAnimationWake  = 2;

private:
    struct Sleeper {
        BlockPos head{};
        i32      counter{0};
    };

    struct Bolt {
        i32                 id{0};
        Vec3d               at{};
        gameplay::BoltClock clock{};
        std::vector<i32>    hit;  // players and mobs struck, so each is hurt once
    };

    struct BedRequest {
        i32      player{0};
        BlockPos clicked{};
    };

    void precipitation(ServerLevel& level, world::ChunkMap& chunks, ChunkPos chunk,
                       bool raining, i32 snow_height, WeatherStats& stats);
    void lightning(ServerLevel& level, world::ChunkMap& chunks, ChunkPos chunk,
                   const cmd::Weather& weather, entity::EntityWorld* mobs,
                   const WeatherHost& host, WeatherStats& stats);
    void tick_bolts(ServerLevel& level, entity::EntityWorld* mobs, const WeatherHost& host,
                    const cmd::WorldState& world, WeatherStats& stats);
    void strike(Bolt& bolt, ServerLevel& level, entity::EntityWorld* mobs,
                const WeatherHost& host, const cmd::WorldState& world, WeatherStats& stats);
    void answer_beds(ServerLevel& level, cmd::WorldState& world, entity::EntityWorld* mobs,
                     const WeatherHost& host, WeatherStats& stats);
    void tick_sleepers(ServerLevel& level, cmd::WorldState& world, const WeatherHost& host,
                       WeatherStats& stats);
    void lie_down(ServerLevel& level, const WeatherPlayer& who, const gameplay::Bed& bed,
                  const WeatherHost& host);
    void wake(ServerLevel& level, i32 player, bool animate, bool announce_change,
              const WeatherHost& host, i32 percentage);
    void announce(const WeatherHost& host, i32 percentage);
    void message(const WeatherHost& host, i32 player, std::string_view key, bool overlay) const;
    [[nodiscard]] bool monsters_near(const AABB& box, entity::EntityWorld* mobs) const;

    [[nodiscard]] const world::Chunk* chunk_of(const world::ChunkMap& chunks, BlockPos pos) const;
    [[nodiscard]] gameplay::BiomeClimate climate_at(const world::Chunk& chunk, BlockPos pos) const;
    [[nodiscard]] u8 light_at(const world::Chunk& chunk, BlockPos pos, bool sky) const;
    [[nodiscard]] i32 surface(const world::Chunk& chunk, i32 x, i32 z, bool motion) const;
    [[nodiscard]] WeatherPlayer const* find(i32 player) const;

    const registry::BlockRegistry*  blocks_;
    const registry::Registries*     registries_;
    MobCombat*                      mob_combat_;
    gameplay::ClimateNoise          climate_;
    gameplay::PrecipitationRules    precipitation_;
    gameplay::BedRules              beds_;
    gameplay::DamageConstants       damage_constants_{};
    math::LegacyRandomSource        random_;
    math::LegacyRandomSource        bolt_random_;
    math::XoroshiroRandomSource     loot_random_{0x424F4C54ULL, 0x4C4F4F54ULL};
    bool                            announce_{true};
    i32                             thunder_chance_{gameplay::kThunderChance};

    registry::BlockId                   rod_{};
    bool                                rod_known_{false};
    i32                                 bolt_type_{-1};
    std::optional<registry::RegistryId> entity_types_;

    /// Said once each, never per bolt.
    bool horse_named_{false};
    bool fire_named_{false};
    bool rod_named_{false};
    bool mooshroom_named_{false};

    struct Candidate {
        Vec3d at;
    };

    std::vector<Bolt>                         bolts_;
    std::unordered_map<i32, Sleeper>          sleepers_;
    std::unordered_map<std::string, BlockPos> bed_spawns_;  // by uuid string
    std::vector<WeatherPlayer>                players_;
    std::vector<BlockPos>                     rods_;
    std::vector<Candidate>                    candidates_;
    std::vector<i32>                          waking_;
    std::vector<gameplay::Drop>               loot_drops_;

    /// The last announced (active, sleeping) pair, for the "changed" test the
    /// game announces on.
    i32 last_active_{0};
    i32 last_sleeping_{0};

    mutable std::mutex      requests_mutex_;
    std::vector<BedRequest> bed_requests_;
    std::vector<i32>        leave_requests_;
    std::vector<BedRequest> bed_taking_;
    std::vector<i32>        leave_taking_;
    /// Mirrors `sleepers_` for the network thread.
    std::vector<i32>        sleeping_ids_;
};

}  // namespace ov::server
