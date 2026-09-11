// The dragon fight: the dragon, its crystals, its fireballs and breath, its
// death, its save, its respawn.
//
// The dragon's own rules — the node graph, the phases, the flight, the damage
// — are ov_gameplay's (dragon.hpp). What is here is the part that has
// entities, packets and blocks: the crystals that heal it and explode, the
// fireballs and the clouds they leave, the blocks its body breaks and the
// players its wings throw, the experience its death drops, the exit portal and
// the gateways, the `DragonFight` compound of level.dat, the respawn with four
// crystals. Sources: the Minecraft Wiki's "Ender Dragon", "End crystal", "End
// podium" and "Lingering potion" pages (Java 1.20), and the real server,
// measured (scripts/measure_dragon.py, docs/provenance/dragon.md).
//
// ── What is not the game's, named ───────────────────────────────────────────
//
//   * the dragon and the crystals are seen by every player in the End within
//     192 blocks of (0, 128, 0); the game tracks the dragon within 160 blocks of
//     each player (measured: a probe at the spawn platform saw it leave and come
//     back fourteen times in five minutes);
//   * the blocks a crystal's blast or the dragon's body breaks drop nothing
//     (the game drops a container's contents); arrows and tridents do not hit
//     the dragon or a crystal (the projectiles of this server live in the
//     overworld's entity world);
//   * the dragon's health and position are not saved: a restart with a living
//     dragon brings a new one at (0, 128, 0) with full health, keeping its UUID;
//   * orbs dropped in the End do not merge, and are not saved.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Everything is called with the server's `players_mutex` held. `hurt`,
// `place_crystal` and `take_breath` run on the network thread and never write a
// block: a crystal's explosion and a respawn are queued and carried out by the
// next `tick`, on the tick thread, the End's only writer.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/brewing.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/dragon.hpp"
#include "ov/gameplay/end_portal.hpp"
#include "ov/gameplay/explosion.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/nbt/tag.hpp"

#include "entity_storage.hpp"  // ── persistence ──
#include "ov/protocol/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/level.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::server {

/// Boss Bar (clientbound 0x0B in protocol 763).
inline constexpr i32 kBossBarPacket = 0x0B;

/// The dragon's maximum health.
inline constexpr f32 kDragonMaxHealth = 200.0F;

/// Ticks the death animation takes before the exit portal opens.
inline constexpr i32 kDragonDeathTicks = gameplay::kDragonDeathAnimation;

/// The respawn sequence, start to the crystals' last blast (wiki: 604 ticks).
inline constexpr i32 kDragonRespawnTicks = 604;

/// The wire ids the fight needs — Mojang's, resolved by name by the caller.
struct EndFightTypes {
    i32 dragon{27};
    i32 crystal{26};
    i32 fireball{22};
    i32 cloud{1};
    i32 experience_orb{34};
    /// `minecraft:dragon_breath` in `minecraft:particle_type`.
    i32 breath_particle{8};
};

/// A player in the End, as the fight sees one.
struct EndFightPlayer {
    i32   id{0};
    Vec3d feet{};
    /// Survival or adventure, and alive: the dragon may target it, the fight
    /// may hurt it. A creative player is still pushed.
    bool mortal{true};
    bool alive{true};
};

struct EndFightHost {
    /// To every player in the End.
    std::function<void(i32 id, std::span<const u8> payload)> broadcast;
    /// Reserve `count` consecutive entity ids and return the first.
    std::function<i32(i32 count)> reserve_entity_ids;
    /// The players in the End. Fills a vector the fight reuses.
    std::function<void(std::vector<EndFightPlayer>&)> players;
    /// To one player's connection (an Explosion's knockback is per player).
    std::function<void(i32 player, i32 id, std::span<const u8> payload)> send_to;
    /// Hurt a player. True when it landed.
    std::function<bool(i32 player, f32 amount, gameplay::DamageKind kind)> hurt_player;
    /// A player picked up an orb.
    std::function<void(i32 player, i32 value)> award_experience;
};

/// One of the fight's entities, for the command engine's selectors.
struct EndFightEntity {
    i32              id{0};
    std::string_view type;
    net::Uuid        uuid{};
    Vec3d            position{};
    f32              width{0.0F};
    f32              height{0.0F};
};

class EndFight final : public LooseAdopter {
public:
    /// `immune` and `transparent` are the blocks of `#dragon_immune` and
    /// `#dragon_transparent`: the body breaks neither; the first stops it.
    EndFight(const gameplay::EndPortalRules& rules, const registry::BlockRegistry& blocks, i64 seed,
             EndFightTypes types, std::span<const registry::BlockId> immune,
             std::span<const registry::BlockId> transparent);

    // ── State ───────────────────────────────────────────────────────────────

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool dragon_alive() const noexcept { return dragon_.has_value(); }
    [[nodiscard]] bool dragon_killed() const noexcept { return killed_; }
    [[nodiscard]] bool previously_killed() const noexcept { return previously_killed_; }
    [[nodiscard]] std::optional<BlockPos> portal() const noexcept { return portal_; }
    [[nodiscard]] f32 dragon_health() const noexcept;
    [[nodiscard]] i32 dragon_id() const noexcept { return dragon_id_; }
    [[nodiscard]] Vec3d dragon_position() const noexcept;
    [[nodiscard]] const gameplay::Dragon* dragon() const noexcept {
        return dragon_ ? &*dragon_ : nullptr;
    }
    [[nodiscard]] gameplay::Dragon* dragon() noexcept { return dragon_ ? &*dragon_ : nullptr; }
    [[nodiscard]] usize crystals_alive() const noexcept;
    /// The fight's crystal count, as the dragon reads it: counted round the
    /// spikes every 100 ticks and on each destruction; 0 until the first count.
    [[nodiscard]] i32 crystal_count() const noexcept { return crystal_count_; }
    [[nodiscard]] std::optional<BlockPos> last_gateway() const noexcept { return last_gateway_; }
    [[nodiscard]] usize fireballs() const noexcept { return fireballs_.size(); }
    [[nodiscard]] usize clouds() const noexcept { return clouds_.size(); }
    [[nodiscard]] usize orbs() const noexcept { return orbs_.size(); }
    [[nodiscard]] i32 experience_dropped() const noexcept { return experience_dropped_; }
    /// Ticks into a respawn, or -1.
    [[nodiscard]] i32 respawn_ticks() const noexcept { return respawn_ticks_; }
    [[nodiscard]] std::span<const i32> gateways() const noexcept { return gateways_; }

    // ── Save ────────────────────────────────────────────────────────────────

    /// Read level.dat's `DragonFight`. A world vanilla wrote reads back.
    void load(const nbt::Tag& fight);

    /// The `DragonFight` compound, keys and types as the real server writes
    /// them (measured): `NeedsStateScanning`, `ExitPortalLocation` (int array),
    /// `Gateways` (int array), `DragonKilled`, `PreviouslyKilled`, `Dragon`
    /// (int-array UUID, kept after a death). No `IsRespawning`: measured
    /// absent eight seconds into a respawn.
    [[nodiscard]] nbt::Tag save() const;

    // ── The fight ───────────────────────────────────────────────────────────

    /// The first player is in the End. `origin_top` is the heightmap
    /// (`MOTION_BLOCKING_NO_LEAVES`) at (0, 0). Writes through `level`.
    void start(world::LevelWriter& level, i32 origin_top, const EndFightHost& host);

    /// Everything a player who now sees the fight must be sent.
    void show_to(const std::function<void(i32, std::span<const u8>)>& send) const;

    /// The boss bar's removal, for a player who no longer sees the fight.
    void hide_from(const std::function<void(i32, std::span<const u8>)>& send) const;

    /// Within 192 blocks of (0, 128, 0), as the game's.
    [[nodiscard]] static bool within_range(Vec3d position) noexcept;

    /// One tick: the dragon, the crystals, the fireballs, the clouds, the
    /// orbs, a queued blast or respawn, the death.
    void tick(world::LevelWriter& level, const EndFightHost& host);

    /// Line of sight for the dragon, blocks only. Optional.
    void set_sight(std::function<bool(Vec3d, Vec3d)> sees) {
        sees_       = std::move(sees);
        world_.sees = sees_;
    }

    /// A hit on one of the fight's entities. False for an id it does not own.
    /// `attacker` is the player who struck, when a player did; `mortal`
    /// whether that player is in survival or adventure.
    bool hurt(i32 entity_id, f32 damage, const EndFightHost& host,
              std::optional<i32> attacker = std::nullopt, bool mortal = true);

    /// `/kill`: the dragon goes without its death animation (measured: no
    /// orbs, the portal opens at once — here on the next tick, the End's
    /// writer); a crystal just goes. False for an id the fight does not own.
    bool kill(i32 entity_id, const EndFightHost& host);

    /// Does the fight own this entity id?
    [[nodiscard]] bool owns(i32 entity_id) const noexcept;

    /// The fight's entities, for `@e`.
    void entities(std::vector<EndFightEntity>& out) const;

    /// An End crystal item used on the top of `on` (obsidian or bedrock, two
    /// free blocks above, checked by the caller). The crystal appears at once;
    /// four round the exit portal start a respawn on the next tick.
    void place_crystal(BlockPos on, const EndFightHost& host);

    /// A glass bottle used by a player whose feet are at `feet`: true when a
    /// breath cloud within reach filled it (the cloud shrinks by half a block).
    bool take_breath(Vec3d feet);

    // ── persistence: DIM1/entities, a LooseAdopter ──────────────────────────
    //
    // `minecraft:ender_dragon`: a living entity's keys, `Health`,
    // `DragonPhase` (int), `DragonDeathTime` (int); `minecraft:end_crystal`:
    // `ShowBottom` (byte), `BeamTarget` {X, Y, Z} (ints) — measured on the
    // real server. A crystal that was destroyed is not in the file, so it does
    // not come back; the dragon comes back where it was, with its health and
    // phase. The fight's fireballs, clouds and orbs are not saved (named).

    /// What an entity read from disk is given its ids with and announced
    /// through. Not owned; must outlive the last read.
    void set_host(const EndFightHost* host) noexcept { host_ = host; }

    /// Has the End's storage read the four chunks round the origin? Until it
    /// has, a fight read from level.dat with a living dragon waits for the
    /// dragon to come back from disk rather than making a new one.
    void set_arena_read(bool read) noexcept { arena_read_ = read; }

    /// Ticks a restored fight waits, arena read, for its dragon to come back
    /// from disk before it makes a new one. Ours, not the game's.
    static constexpr i32 kDragonRestoreWait = 100;

    [[nodiscard]] bool owns_type(std::string_view type) const noexcept override {
        return type == "minecraft:ender_dragon" || type == "minecraft:end_crystal";
    }
    bool adopt_saved(const nbt::Tag& compound) override;
    void positions(std::vector<Vec3d>& out) const override;
    void save(std::vector<LooseEntity>& out) const override;
    void release(const std::function<bool(ChunkPos)>& leaving, std::vector<LooseEntity>& out,
                 std::vector<i32>& removed) override;

    /// The dragon's compound, as vanilla writes it. Nullopt with no dragon.
    [[nodiscard]] std::optional<nbt::Tag> dragon_nbt() const;
    /// Is the fight waiting for its dragon to come back from disk?
    [[nodiscard]] bool awaiting_dragon() const noexcept { return awaiting_dragon_; }

private:
    struct Crystal {
        i32                     entity_id{0};
        net::Uuid               uuid{};
        Vec3d                   position{};
        bool                    alive{true};
        bool                    show_bottom{true};
        /// Put by a player (the respawn's four, among others).
        bool                    placed{false};
        std::optional<BlockPos> beam;
    };
    struct Fireball {
        i32       entity_id{0};
        net::Uuid uuid{};
        Vec3d     position{};
        Vec3d     velocity{};
        Vec3d     power{};
        i32       age{0};
    };
    struct CloudEntity {
        i32             entity_id{0};
        net::Uuid       uuid{};
        Vec3d           position{};
        gameplay::Cloud cloud{};
        /// Harming's amplifier: 1 for a fireball's cloud, 0 for the breath.
        i32  amplifier{1};
        bool breath{false};
        /// When each player may be affected again, by player id.
        std::vector<std::pair<i32, i32>> reapply;
        f32  sent_radius{-1.0F};
        bool sent_waiting{true};
    };
    struct Orb {
        i32   entity_id{0};
        Vec3d position{};
        Vec3d velocity{};
        i32   value{0};
        i32   age{0};
    };
    struct PendingBlast {
        Vec3d              centre{};
        std::optional<i32> attacker;
        /// 6 for a crystal; 5 for a pillar's top during a respawn (measured).
        f32 power{gameplay::kEndCrystalPower};
        /// The respawn's four crystals blow without touching a block
        /// (measured: 0 records).
        bool keep_blocks{false};
    };

    void spawn_dragon(const EndFightHost& host);
    /// The dragon's spawn, metadata and boss bar. ── persistence ──
    void send_dragon(const std::function<void(i32, std::span<const u8>)>& send) const;
    [[nodiscard]] nbt::Tag crystal_nbt(const Crystal& crystal) const;
    void set_health_packets(const EndFightHost& host);
    void send_phase(const EndFightHost& host);
    /// The lethal hit's packets, when `before` / `before_health` say it was.
    void death_packets(const EndFightHost& host, gameplay::DragonPhase before, f32 before_health);
    [[nodiscard]] std::optional<i32> strafe_target(std::optional<i32> attacker, bool mortal,
                                                   Vec3d at) const;
    void finish_death(world::LevelWriter& level, const EndFightHost& host, bool animation);
    void tick_dragon(world::LevelWriter& level, const EndFightHost& host);
    void tick_crystals(world::LevelWriter& level, const EndFightHost& host);
    void tick_fireballs(world::LevelWriter& level, const EndFightHost& host);
    void tick_clouds(const EndFightHost& host);
    void tick_orbs(const world::LevelView& level, const EndFightHost& host);
    void tick_respawn(world::LevelWriter& level, const EndFightHost& host);
    void body_against_world(world::LevelWriter& level, const EndFightHost& host);
    void blast(world::LevelWriter& level, const PendingBlast& blast, const EndFightHost& host);
    void destroy_crystal(usize index, bool explode, std::optional<i32> strafe,
                         std::optional<i32> attacker, const EndFightHost& host);
    void spawn_cloud(Vec3d at, bool breath, const EndFightHost& host);
    void spawn_orbs(Vec3d at, i32 amount, const EndFightHost& host);
    void spawn_fireball(Vec3d from, Vec3d direction, const EndFightHost& host);
    void send_crystal(const Crystal& crystal,
                      const std::function<void(i32, std::span<const u8>)>& send) const;
    void send_cloud(const CloudEntity& cloud,
                    const std::function<void(i32, std::span<const u8>)>& send) const;
    void recount_crystals();
    void rebuild_spike(world::LevelWriter& level, usize spike) const;
    [[nodiscard]] bool immune(registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool transparent(registry::BlockStateId state) const noexcept;
    [[nodiscard]] std::vector<u8> boss_bar_add() const;
    [[nodiscard]] net::Uuid next_uuid();

    const gameplay::EndPortalRules* rules_;
    const registry::BlockRegistry*  blocks_;
    i64                             seed_;
    EndFightTypes                   types_;
    std::vector<bool>               immune_;
    std::vector<bool>               transparent_;
    gameplay::Explosions            explosions_;
    std::function<bool(Vec3d, Vec3d)> sees_;
    /// What the dragon reads each tick; kept so the tick copies nothing.
    gameplay::DragonSurroundings    world_;
    registry::BlockStateId          fire_{};
    registry::BlockStateId          obsidian_{};
    registry::BlockStateId          bedrock_{};
    registry::BlockStateId          iron_bars_{};

    bool                    started_{false};
    bool                    killed_{false};
    bool                    previously_killed_{false};
    bool                    needs_scanning_{true};
    std::optional<BlockPos> portal_;
    std::optional<BlockPos> last_gateway_;
    /// The gateways still to open, as level.dat stores them: indices into the
    /// twenty positions, the next one at the back.
    std::vector<i32> gateways_;

    std::optional<gameplay::Dragon> dragon_;
    i32                             dragon_id_{0};
    std::optional<net::Uuid>        dragon_uuid_;
    net::Uuid                       bar_uuid_{};
    f32                             sent_health_{-1.0F};
    i64                             ticks_{0};
    /// The crystal the dragon is drawing on, or -1.
    i32 nearest_{-1};
    i32 crystal_count_{0};
    i32 count_timer_{0};
    i32 fountain_{64};
    i32 experience_dropped_{0};

    std::vector<Crystal>      crystals_;
    std::vector<Fireball>     fireballs_;
    std::vector<CloudEntity>  clouds_;
    std::vector<Orb>          orbs_;
    std::vector<PendingBlast> blasts_;
    std::vector<EndFightPlayer> players_;
    std::vector<gameplay::DragonPlayer> targets_;
    std::vector<BlockPos>     cells_;
    std::vector<BlockPos>     air_;
    std::vector<i32>          gone_;

    /// The respawn: -1 when none; otherwise ticks since it began, and the four
    /// crystals' indices.
    i32                  respawn_ticks_{-1};
    std::array<usize, 4> respawn_crystals_{};
    bool                 respawn_check_{false};
    /// A `/kill` of the dragon, carried out by the next tick.
    bool                 kill_pending_{false};

    // ── persistence ──
    const EndFightHost* host_{nullptr};
    /// A fight with a living dragon that is on disk, not here.
    bool awaiting_dragon_{false};
    bool arena_read_{false};
    i32  awaiting_ticks_{0};
    /// The compound the dragon was read with: what is not modelled goes back.
    nbt::Tag dragon_saved_{};

    /// The fight's own draws (which crystal is looked for when, the dragon's
    /// phases, the orbs' scatter). Seeded from the world seed; not the game's
    /// stream.
    math::LegacyRandomSource random_;
};

}  // namespace ov::server
