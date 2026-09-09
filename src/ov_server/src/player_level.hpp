// The world as a player's right-click writes it.
//
// `ItemUse` takes a `world::LevelWriter&` — layer 9 must be able to run a door
// against a test map, so it cannot take a server. The server already owns one
// such writer, `ServerLevel`, and it is the wrong one for this job: it belongs
// to the tick thread, it records every write into `changed_` for the drain to
// settle, and its `set_block` hook is the drain's own — it assumes the chunk
// lock is already held and defers relighting.
//
// A player's interaction arrives on the **network** thread, inside the packet
// switch, and has to behave like a placement: take the chunk lock per access,
// broadcast the Block Update itself, and queue the neighbour notification for
// the next tick. That is what this adapter is, and it is deliberately thin —
// four hooks and no state beyond a write count.
//
// ── The one thing that cannot be done here ──────────────────────────────────
//
// A button schedules a block tick 30 ticks out, and the queue that tick lands
// in lives inside `ServerLevel`, on the tick thread. `BlockTickScheduler` is
// not thread-safe. It is reachable all the same, because *every* access to it
// in this server — the drain, the chunk loader, the save — happens under
// `chunk_mutex`; so `schedule_tick` here takes that same lock and writes
// straight into the real queue. The hook is a `std::function` for exactly that
// reason: this file must not know what `chunk_mutex` is.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/block_ticks.hpp"
#include "ov/world/level.hpp"

#include <functional>
#include <string_view>

namespace ov::server {

/// What a player's writer reaches outside itself for.
///
/// Callbacks rather than a reference to the server, for the same reason
/// `LevelHooks` and `WorkbenchHost` are: this file must not learn what a chunk
/// map, a connection or a mutex is.
struct PlayerLevelHooks {
    /// The state at a position. Takes the chunk lock itself.
    std::function<registry::BlockStateId(BlockPos)> block_at;

    /// Is the chunk holding this position resident?
    std::function<bool(BlockPos)> is_loaded;

    /// Write, tell every client, and queue the neighbour notification.
    ///
    /// Called with the chunk lock **released** — the server's own
    /// `set_block_and_broadcast` takes it, and it is not recursive.
    std::function<void(BlockPos, registry::BlockStateId)> set_block;

    /// Put a tick in the real queue, under the chunk lock.
    std::function<void(BlockPos, std::string_view, i64, world::TickQueue, world::TickPriority)>
        schedule_tick;

    /// Ask the real queue. Under the chunk lock, like the above.
    std::function<bool(BlockPos, std::string_view, world::TickQueue)> has_scheduled_tick;

    /// The tick the server is on. Read from an atomic, never from a clock.
    std::function<i64()> game_time;

    /// Answers `blocks()`. Must outlive the adapter.
    const registry::BlockRegistry* blocks{nullptr};
};

/// A `LevelWriter` over the server's chunks, safe to use from a packet handler.
///
/// One per interaction is fine — it holds nothing that has to survive between
/// two of them — but the hooks capture the server's locals, so in practice the
/// server builds one and reuses it.
class PlayerLevel final : public world::LevelWriter {
public:
    explicit PlayerLevel(PlayerLevelHooks hooks);

    // ── LevelView ───────────────────────────────────────────────────────────

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override;
    [[nodiscard]] bool                   is_loaded(BlockPos pos) const override;
    [[nodiscard]] world::WorldShape      shape() const override { return shape_; }
    [[nodiscard]] world::DimensionTraits traits() const override { return traits_; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *hooks_.blocks; }

    // ── LevelWriter ─────────────────────────────────────────────────────────

    void set_block(BlockPos pos, registry::BlockStateId state) override;

    void schedule_tick(BlockPos pos, std::string_view what, i64 delay, world::TickQueue queue,
                       world::TickPriority priority) override;

    [[nodiscard]] bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                          world::TickQueue queue) const override;

    [[nodiscard]] i64 game_time() const override;

    // ── Instrumentation ─────────────────────────────────────────────────────

    /// How many blocks the last interaction wrote. The server reads it to tell
    /// "the rules did nothing" from "the rules wrote the same state back",
    /// which are different answers to "why did the door not move".
    [[nodiscard]] u32 writes() const noexcept { return writes_; }
    void              clear_writes() noexcept { writes_ = 0; }

private:
    PlayerLevelHooks       hooks_;
    world::WorldShape      shape_{world::WorldShape::overworld()};
    world::DimensionTraits traits_{};
    u32                    writes_{0};
};

}  // namespace ov::server
