// Blocks that asked to be woken up later.
//
// A redstone repeater with a delay of two, a piston finishing its push, a fluid
// deciding where to go next — none of them can act in the tick that changed
// them. They schedule themselves, and something has to keep the queue.
//
// Three things about that queue are load-bearing, and none is obvious:
//
//   * **Priority decides the order inside a single tick.** Vanilla runs from -3
//     to 2, and the numbers are not decoration: a repeater turning off asks for
//     -3 so it lands before the one turning on at -2, and swapping them changes
//     what a real circuit does. Ordering by position, or by insertion alone,
//     builds a machine that works until someone puts two repeaters side by side.
//
//   * **A tie is broken by insertion order, not by position.** Two ticks with
//     the same target tick and the same priority run in the order they were
//     asked for. That is why a sequence number is stored rather than derived.
//
//   * **One tick per position per block.** Asking twice for the same block at
//     the same position is a no-op, which is what keeps a wire that sees six
//     neighbour updates from scheduling six ticks. Vanilla keys on both, so a
//     water tick and a repeater tick at the same position coexist.
//
// This type knows nothing about redstone, fluids, or what a tick *does*. It is
// a queue with a save format. The behaviour lives above, in ov_gameplay.
#pragma once

#include "ov/math/block_pos.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::world {

/// Vanilla's tick priorities, which run from -3 to 2 inclusive.
///
/// Named because the raw numbers appear in circuits' behaviour and a bare `-3`
/// at a call site says nothing. The values are the ones written to disk.
enum class TickPriority : i8 {
    ExtremelyHigh = -3,
    VeryHigh      = -2,
    High          = -1,
    Normal        = 0,
    Low           = 1,
    VeryLow       = 2,
};

inline constexpr i8 kMinTickPriority = -3;
inline constexpr i8 kMaxTickPriority = 2;

/// One entry of the queue.
struct ScheduledTick {
    BlockPos          pos{};
    registry::BlockId block{0};
    /// The absolute game tick this fires on. Absolute rather than a countdown:
    /// a countdown has to be decremented on every entry every tick, which is
    /// the whole queue every tick for no reason.
    i64          trigger_tick{0};
    TickPriority priority{TickPriority::Normal};
    /// Insertion sequence, the tie-break. Never reused, never wraps in any
    /// realistic run: at a million schedules a second it lasts half a million
    /// years.
    u64 order{0};

};

/// The order vanilla runs them in: soonest first, then by priority, then by the
/// order they were asked for.
///
/// A named comparator rather than `operator<`: these are not values with a
/// natural ordering, and giving them one invites a sort somewhere that means
/// something else.
[[nodiscard]] inline bool tick_order(const ScheduledTick& a, const ScheduledTick& b) noexcept {
    if (a.trigger_tick != b.trigger_tick) {
        return a.trigger_tick < b.trigger_tick;
    }
    if (a.priority != b.priority) {
        return static_cast<i8>(a.priority) < static_cast<i8>(b.priority);
    }
    return a.order < b.order;
}

/// How many ticks vanilla is willing to run in one game tick before it gives up
/// and leaves the rest for later. Exceeding it is how an infinite circuit stays
/// merely laggy instead of hanging the server.
inline constexpr usize kMaxTicksPerGameTick = 65536;

/// The queue of pending block ticks for one dimension.
///
/// Not per chunk, unlike the save format. A single writer owns the world, so a
/// single queue is both simpler and faster; `save_chunk` and `load_chunk`
/// bridge to the per-chunk layout on disk.
class BlockTickScheduler {
public:
    /// Ask for a tick `delay` ticks from `now`. A delay of zero fires on the
    /// next drain, not this one.
    ///
    /// Returns false when this position already has a tick pending for this
    /// block, in which case nothing changes — the first request wins, which is
    /// what vanilla does and what keeps a repeater's delay from being reset by
    /// a neighbour twitching.
    bool schedule(BlockPos pos, registry::BlockId block, i64 now, i32 delay,
                  TickPriority priority = TickPriority::Normal);

    /// Schedule at an absolute tick, for loading a save.
    bool schedule_at(BlockPos pos, registry::BlockId block, i64 trigger_tick,
                     TickPriority priority = TickPriority::Normal);

    [[nodiscard]] bool is_scheduled(BlockPos pos, registry::BlockId block) const noexcept;

    /// Drop the pending tick for this position and block, if there is one.
    ///
    /// What breaking the block has to do. A tick surviving its block fires on
    /// whatever replaced it.
    bool cancel(BlockPos pos, registry::BlockId block);

    /// Drop every pending tick at this position, whatever the block.
    usize cancel_all_at(BlockPos pos);

    [[nodiscard]] usize pending_count() const noexcept { return entries_.size(); }

    /// Take every tick due at or before `now`, sorted, and remove them from the
    /// queue. Appends to `out`.
    ///
    /// Taking them all out first — rather than running them off the queue one
    /// at a time — is deliberate: a tick that reschedules itself for the same
    /// game tick would otherwise run again inside the same drain and a lamp
    /// would flicker forever without the game advancing.
    void drain_due(i64 now, std::vector<ScheduledTick>& out, usize limit = kMaxTicksPerGameTick);

    /// Every pending tick, sorted. For tests and for saving.
    void snapshot(std::vector<ScheduledTick>& out) const;

    void clear() noexcept;

    // ── The Anvil save format ───────────────────────────────────────────────
    //
    // On disk each chunk carries its own `block_ticks` list, and each entry is
    //
    //     {i: "minecraft:repeater", p: 0, t: 3, x: 12, y: 65, z: -40}
    //
    // `i` is the block's registry **name** — the same reason the palette is
    // name-based, so a save survives a version whose numeric ids moved. `t` is
    // a **delay relative to the level's gameTime at the moment of saving**, not
    // an absolute tick; a world reloaded a month later must not fire a month of
    // backlog at once. Both facts were checked against a real 1.20.1 save, see
    // docs/provenance/redstone.md.

    /// The ticks belonging to one chunk, as the Anvil list, relative to
    /// `game_time`. Returns an empty TAG_List of compounds when there are none,
    /// which is what vanilla writes.
    [[nodiscard]] nbt::Tag save_chunk(ChunkPos chunk, i64 game_time,
                                      const registry::BlockRegistry& blocks) const;

    /// Read one chunk's `block_ticks` list back, resolving `t` against
    /// `game_time`.
    ///
    /// Returns the number of entries loaded. An entry naming a block this
    /// version does not have is **refused and counted** rather than dropped
    /// silently: `skipped` receives it, and a caller that ignores a non-zero
    /// skip count is loading a world it cannot run.
    usize load_chunk(const nbt::Tag& list, i64 game_time, const registry::BlockRegistry& blocks,
                     usize& skipped);

private:
    /// Key of the "one tick per position per block" rule.
    struct Key {
        BlockPos pos;
        u16      block;

        [[nodiscard]] friend bool operator==(const Key&, const Key&) noexcept = default;
    };

    struct KeyHash {
        [[nodiscard]] usize operator()(const Key& key) const noexcept;
    };

    std::unordered_set<Key, KeyHash> pending_;
    /// Unsorted; sorted on drain. A heap would keep it ordered at every insert,
    /// but a tick that is cancelled has to come out of the middle, and the
    /// number due in one game tick is small next to the number pending.
    std::vector<ScheduledTick> entries_;
    u64                        next_order_{0};
};

}  // namespace ov::world
