// The world's alarm clock, wired to the engines that answer it.
//
// `ov_world` has kept a `BlockTickScheduler` since M1 and `ov_gameplay` has had
// a measured fluid engine and a measured redstone engine for as long — and
// until this file existed nothing called any of the three. The server ticked
// entities and streamed chunks; water placed by a bucket sat in a one-block
// cube for ever and a lever lit nothing.
//
// What was missing is not a rule. It is the three lines that join them:
//
//   1. a `LevelWriter` over the server's chunks, so a rule can read and write
//      the real world instead of a test map;
//   2. a drain of the two queues once per tick, fluid ticks to `FluidRules` and
//      block ticks to `Redstone`;
//   3. a neighbour notification after **every** block change, whoever made it —
//      a player, a rule, or the drain itself.
//
// The third is the one that is easy to leave out and impossible to work around.
// Vanilla's `Level.setBlock` notifies the six neighbours as part of writing;
// without it a water source placed next to a hole never learns the hole is
// there, and a wire next to a lever never learns the lever moved. Both engines
// are written expecting it — `FluidRules::on_neighbour_changed` and
// `Redstone::neighbour_changed` are public for exactly this caller.
//
// ── Why the notification is a queue and not recursion ───────────────────────
//
// Vanilla recurses: `setBlock` calls `neighborChanged`, which calls `setBlock`.
// On a long wire that is a stack a thousand frames deep, and `update_wire`
// already declined to do it that way for the same reason. So a write here only
// **records** its position, and the driver drains the record in waves after the
// rule returns. The order inside one wave is write order, which is the order
// vanilla's recursion would have reached them in for everything that does not
// branch, and the engines that do branch — the wire — resolve their own
// propagation internally before returning.
#pragma once

#include "ov/gameplay/fluid.hpp"
#include "ov/gameplay/nether_portal.hpp"  // ── nether ──
#include "ov/gameplay/plants.hpp"
#include "ov/gameplay/redstone.hpp"
#include "ov/gameplay/signal.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/block_ticks.hpp"
#include "ov/world/level.hpp"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ov::server {

/// The four things the level adapter reaches outside itself for.
///
/// Callbacks rather than a reference to the server, for the same reason
/// `WorkbenchHost` is: this file must not know what a chunk map, a connection
/// or a player is, and the server's own helpers are lambdas over its locals.
struct LevelHooks {
    /// The state at a position. Air outside the world and in a chunk that is
    /// not resident — which is why `is_loaded` exists beside it.
    std::function<registry::BlockStateId(BlockPos)> block_at;

    /// Is the chunk holding this position resident?
    ///
    /// Fluids ask, and the answer must be honest: flowing into a chunk that is
    /// merely *not loaded yet* would carve a puddle into terrain that has not
    /// been generated, and the result depends on where the player walked.
    std::function<bool(BlockPos)> is_loaded;

    /// Write a state and tell every client. Whatever the server does around a
    /// change — heightmaps, light, dirty flags — happens in here.
    std::function<void(BlockPos, registry::BlockStateId)> set_block;

    /// A comparator's reading of the container at a position, or -1 when there
    /// is no container there. See `RedstoneWorld::container_signal`.
    std::function<i32(BlockPos)> container_signal;
};

/// The server's world, as a block behaviour sees it.
///
/// Owns the two queues. They belong to the level rather than to a chunk because
/// vanilla's do: a tick is addressed by absolute position and is only sorted
/// into a chunk when the chunk is written to disk.
class ServerLevel final : public gameplay::RedstoneWorld {
public:
    ServerLevel(const registry::BlockRegistry& blocks, LevelHooks hooks);

    // ── LevelView ───────────────────────────────────────────────────────────

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override;
    [[nodiscard]] bool                   is_loaded(BlockPos pos) const override;
    [[nodiscard]] world::WorldShape      shape() const override { return shape_; }
    [[nodiscard]] world::DimensionTraits traits() const override { return traits_; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }

    // ── LevelWriter ─────────────────────────────────────────────────────────

    void set_block(BlockPos pos, registry::BlockStateId state) override;

    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue queue,
                       world::TickPriority priority) override;

    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue queue) const override;

    [[nodiscard]] i64 game_time() const override { return now_; }

    // ── RedstoneWorld ───────────────────────────────────────────────────────

    [[nodiscard]] i32 container_signal(BlockPos pos) const override;

    // ── The server's side of it ─────────────────────────────────────────────

    void set_game_time(i64 now) noexcept { now_ = now; }

    // ── nether ── A level for another dimension: its height and its rules —
    // `ultrawarm` is what makes lava flow as far and as fast as water.
    void set_dimension(world::WorldShape shape, world::DimensionTraits traits) noexcept {
        shape_  = shape;
        traits_ = traits;
    }

    [[nodiscard]] world::BlockTickScheduler&       queue(world::TickQueue which) noexcept;
    [[nodiscard]] const world::BlockTickScheduler& queue(world::TickQueue which) const noexcept;

    /// Positions written since `clear_changed`, in write order.
    [[nodiscard]] std::span<const BlockPos> changed() const noexcept { return changed_; }

    void clear_changed() noexcept { changed_.clear(); }

    /// Treat a position as though it had just been written, without writing it.
    ///
    /// What a player's edit needs: the block is already in the world — the
    /// placement path put it there, with its own connection reshaping and its
    /// own broadcast — and all that is missing is the notification a rule's
    /// write would have produced. Writing it again here would relight and
    /// rebroadcast an unchanged block once per edit.
    void mark_changed(BlockPos pos) { changed_.push_back(pos); }

    /// Total writes since the level was built. Instrumentation, never a
    /// decision.
    [[nodiscard]] u64 writes() const noexcept { return writes_; }

private:
    const registry::BlockRegistry* blocks_{nullptr};
    LevelHooks                     hooks_;

    world::BlockTickScheduler block_ticks_;
    world::BlockTickScheduler fluid_ticks_;

    world::WorldShape      shape_{world::WorldShape::overworld()};
    world::DimensionTraits traits_{};
    i64                    now_{0};

    /// Reused between waves and between ticks. Grows to its working size in the
    /// first seconds and never allocates again, which is what keeps the drain
    /// inside the no-allocation rule.
    std::vector<BlockPos> changed_;
    u64                   writes_{0};
};

// ── tnt and gravity ──────────────────────────────────────────────────────────
/// A third set of block rules, owned outside this file: falling blocks and
/// TNT (tnt_gravity.hpp). An interface rather than a member so this file does
/// not learn what an entity is — both rules end by asking for one.
class BlockRuleExtension {
public:
    BlockRuleExtension()                                     = default;
    BlockRuleExtension(const BlockRuleExtension&)            = delete;
    BlockRuleExtension& operator=(const BlockRuleExtension&) = delete;
    BlockRuleExtension(BlockRuleExtension&&)                 = delete;
    BlockRuleExtension& operator=(BlockRuleExtension&&)      = delete;
    virtual ~BlockRuleExtension()                            = default;

    /// Called exactly where the fluid engine is: for every changed position
    /// and each of its six neighbours.
    virtual void neighbour_changed(ServerLevel& level, BlockPos pos) = 0;

    /// A block tick came due. True when it was this extension's to answer.
    virtual bool scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) = 0;
};
// ── end tnt and gravity ──────────────────────────────────────────────────────

/// What one drain did. Instrumentation, and the numbers a report is written
/// from.
struct WorldTickStats {
    usize fluid_ticks{0};
    usize block_ticks{0};
    /// Block ticks naming something the registry does not have. Refused rather
    /// than defaulted, and counted so that "never happens" is a measurement.
    usize refused{0};
    usize waves{0};
    usize notifications{0};
};

/// The two queues, drained once per tick, and the engines that answer them.
class WorldTicks {
public:
    WorldTicks(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// Drain everything due at `now`, then settle what it changed.
    WorldTickStats run(ServerLevel& level, i64 now);

    /// A block changed outside the queues — a player placed or broke one, or a
    /// bucket was emptied. Wake whatever was leaning on it.
    ///
    /// The counterpart of vanilla's `updateNeighborsAt`, and the reason a lever
    /// a player flips does anything at all.
    usize notify(ServerLevel& level, BlockPos pos);

    [[nodiscard]] const gameplay::FluidRules& fluid() const noexcept { return fluid_; }
    [[nodiscard]] gameplay::Redstone&         redstone() noexcept { return redstone_; }

    // ── agriculture ──
    /// Let the plant rules answer block ticks and neighbour changes too: a
    /// leaf recomputing its distance, a crop losing its farmland. Both must
    /// outlive this object. See agriculture.hpp.
    void attach_plants(const gameplay::Plants& plants, gameplay::PlantEnvironment& env) noexcept {
        plants_    = &plants;
        plant_env_ = &env;
    }

    /// Settle whatever was written since the level's last clear — the random
    /// tick's writes, which happen outside `run`.
    usize settle_writes(ServerLevel& level) {
        usize waves = 0;
        return settle(level, waves);
    }
    // ── end agriculture ──

    /// How many waves one settle may run before it is called a loop.
    ///
    /// A circuit that never settles is laggy in vanilla too; what it must not
    /// be is unbounded. Reached only by a clock, and a clock is meant to keep
    /// going — so the drain stops for this tick rather than reporting an error.
    static constexpr usize kMaxWaves = 512;

    // ── tnt and gravity ──────────────────────────────────────────────────────
    /// Hand the drain a third engine. Not owned; null turns it off.
    void set_extension(BlockRuleExtension* extension) noexcept { extension_ = extension; }

    /// Notify around everything the level recorded as written since the last
    /// settle — what an explosion that wrote through the level needs, since it
    /// does not happen inside `run`.
    usize settle_changes(ServerLevel& level) {
        usize waves = 0;
        return settle(level, waves);
    }
    // ── end tnt and gravity ──────────────────────────────────────────────────

private:
    /// Notify around everything in `seeds`, and keep going while writes appear.
    usize settle(ServerLevel& level, usize& waves);

    gameplay::FluidRules fluid_;
    gameplay::Redstone   redstone_;

    const gameplay::Plants*     plants_{nullptr};
    gameplay::PlantEnvironment* plant_env_{nullptr};

    std::vector<world::ScheduledTick> due_;
    /// The wave being processed, moved out of the level so that writes made
    /// while notifying land in the *next* wave rather than in this one.
    std::vector<BlockPos> wave_;

    BlockRuleExtension* extension_{nullptr};  // ── tnt and gravity ──

    // ── nether ──
public:
    /// Let every write reach the portal rules: a fire lights its frame, a
    /// broken frame block empties its portal. Not owned; null turns it off.
    void attach_portals(const gameplay::PortalRules* portals) noexcept { portals_ = portals; }

private:
    const gameplay::PortalRules* portals_{nullptr};
    // ── end nether ──
};

}  // namespace ov::server
