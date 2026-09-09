// Scheduled block and fluid ticks: the world's alarm clock.
//
// Most of the game does not run every tick. Water waits 5 ticks before it
// spreads, lava 30 in the Overworld and 10 in the Nether, a repeater 2 per
// setting, leaves an unpredictable while before they decay. Vanilla does not
// scan for these — it keeps a queue of "wake this position up later", and the
// order that queue is drained in is observable: two updates landing on the same
// tick resolve in a defined sequence, and a redstone circuit that resolves them
// in the other order latches the wrong way.
//
// So the ordering rule is part of the format, not an implementation detail.
// Ticks come out sorted by
//
//     (when, priority, insertion order)
//
// — the target tick first, then vanilla's TickPriority (a signed value where
// **lower runs earlier**; 0 is normal), and finally the order they were
// scheduled in, which is what makes the whole thing deterministic. Every one of
// the three is needed: without the priority a piston and its block break tie
// arbitrarily, without the sequence two ticks at the same priority do.
//
// ── Two queues, not one ────────────────────────────────────────────────────
//
// Anvil stores `block_ticks` and `fluid_ticks` as separate lists on the chunk,
// and vanilla drains them separately, so they are kept apart here too. They
// differ in what the `i` field names, and that difference is measured rather
// than assumed — a save from a real 1.20.1 server carries
//
//     block_ticks: {i: "minecraft:oak_leaves", p: 0, t: 0, x: 176, y: -57, z: 189}
//     fluid_ticks: {i: "minecraft:flowing_lava", p: 0, t: 8, x: 719, y: -60, z: -3}
//
// A block tick names the **block**; a fluid tick names the **fluid**, and the
// fluid registry distinguishes `water` from `flowing_water` and `lava` from
// `flowing_lava`. Writing the block name into a fluid tick produces a file
// vanilla loads with every pending flow silently dropped.
//
// `t` is a **delay relative to the chunk's game time**, not an absolute tick.
// That is what lets a chunk sit unloaded for an hour and resume where it left
// off, and it is why loading has to be told what "now" is.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/tag.hpp"

#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::world {

/// Vanilla's TickPriority, as the `p` field stores it.
///
/// Lower runs first. The names are vanilla's own; the numbers are what appear
/// on disk, so they are fixed rather than ours to choose.
enum class TickPriority : i8 {
    ExtremelyHigh = -3,
    VeryHigh      = -2,
    High          = -1,
    Normal        = 0,
    Low           = 1,
    VeryLow       = 2,
    ExtremelyLow  = 3,
};

/// One pending wake-up.
///
/// `what` is a registry name rather than an id because that is what the disk
/// format carries, and because the two queues name things from two different
/// registries — a numeric id here would have to say which registry it came
/// from, and the string already does.
///
/// It is a **view**, not a string, and it is owned by the scheduler that handed
/// it out. A `std::string` here would allocate on every schedule, and a pool
/// under load schedules thousands per tick — straight through the no-allocation
/// rule the tick body is guarded by. The scheduler interns the handful of names
/// that ever appear and every tick borrows one.
struct ScheduledTick {
    BlockPos         pos{};
    std::string_view what;
    i64              when{0};
    TickPriority     priority{TickPriority::Normal};

    /// Insertion order, the final tiebreak. Assigned by the scheduler; a caller
    /// never sets it.
    u64 sequence{0};

    /// The ordering vanilla drains the queue in.
    [[nodiscard]] bool runs_before(const ScheduledTick& other) const noexcept;
};

/// The two queues a level keeps.
enum class TickQueue : u8 { Block, Fluid };

/// A queue of scheduled ticks, ordered the way vanilla drains it.
///
/// Deliberately not a `priority_queue`: draining needs to happen in bulk once
/// per tick, duplicates have to be recognised (scheduling the same position
/// twice is extremely common and vanilla keeps only the first), and the whole
/// thing has to be enumerable for saving. A sorted drain of a flat vector does
/// all three and never allocates in the steady state, because the storage is
/// reused.
class BlockTickScheduler {
public:
    /// Ask for `what` at `pos` to be ticked `delay` ticks from `now`.
    ///
    /// Ignored if that exact position and name is already queued, whatever its
    /// delay — vanilla's `hasScheduledTick` check. Without it a block whose
    /// neighbours all change on one tick schedules itself six times and runs
    /// six times, which for a fluid means a flow that advances six blocks in
    /// one step.
    void schedule(BlockPos pos, std::string_view what, i64 delay, i64 now,
                  TickPriority priority = TickPriority::Normal);

    [[nodiscard]] bool is_scheduled(BlockPos pos, std::string_view what) const noexcept;


    [[nodiscard]] usize pending() const noexcept { return pending_.size(); }

    /// Move every tick due at or before `now` into `out`, in drain order.
    ///
    /// `out` is cleared first and kept between calls by the caller, which is
    /// what keeps this allocation-free once the world has warmed up.
    ///
    /// Ticks scheduled *during* a drain land in the queue for a later tick and
    /// are not returned by this call, even if their delay is zero. That is
    /// vanilla's behaviour and it is what stops a water source and its own
    /// flow from resolving inside one another.
    void collect_due(i64 now, std::vector<ScheduledTick>& out);

    /// Everything still queued, in drain order. For saving.
    [[nodiscard]] std::vector<ScheduledTick> snapshot() const;

    /// Drop everything for one chunk column, for unloading.
    void forget_chunk(i32 chunk_x, i32 chunk_z);

    void clear() noexcept;

private:
    /// Intern a name, returning a view that outlives every tick holding it.
    [[nodiscard]] std::string_view intern(std::string_view name);

    std::vector<ScheduledTick> pending_;
    u64                        next_sequence_{0};

    /// The names ever seen, kept for the life of the scheduler.
    ///
    /// A deque and not a vector: a vector reallocating would move its strings,
    /// and short ones live inside the string object rather than on the heap, so
    /// every view handed out so far would dangle. There are a few dozen of
    /// these in a whole world.
    std::deque<std::string> names_;
};

// ── Anvil ──────────────────────────────────────────────────────────────────
//
// The two lists live at the top level of the chunk compound, named
// `block_ticks` and `fluid_ticks`. Both are lists of
// `{i: String, p: Int, t: Int, x: Int, y: Int, z: Int}` with `t` relative to
// the chunk's own game time.

/// Serialise the ticks falling inside one chunk column.
///
/// `now` turns absolute tick numbers back into the relative delays the format
/// wants. An empty list is still returned rather than nothing; the caller
/// decides whether to write it, since vanilla omits empty lists.
[[nodiscard]] nbt::Tag ticks_to_nbt(std::span<const ScheduledTick> ticks, i32 chunk_x,
                                    i32 chunk_z, i64 now);

/// Read one of the two lists back into a scheduler.
///
/// Loads rather than returns, because a tick's name is interned by the
/// scheduler that holds it — there is nowhere for a free-standing one to live.
/// The order of the list is kept as the tiebreak, which is what vanilla saved.
///
/// Returns false, having loaded nothing, if the tag is not a list of compounds
/// shaped the way the format says. A malformed entry is refused rather than
/// defaulted: a tick with a guessed position is a block that updates somewhere
/// else, and nothing about the world would say where the mistake came from.
[[nodiscard]] bool ticks_from_nbt(const nbt::Tag& list, i64 now, BlockTickScheduler& into);

}  // namespace ov::world
