// Redstone: the wire, the components, and the update storm they make.
//
// The wire is the subtle part. Its state carries five properties — a power from
// 0 to 15 and, per horizontal direction, whether it reaches `up`, `side` or
// `none` — and both halves feed each other: the shape decides who a wire takes
// power from, and the power decides nothing about the shape. Getting the shape
// wrong is invisible on a straight line and wrong on every staircase.
//
// The propagation rule itself is four lines, and each line is load-bearing:
//
//     strength = best signal from any neighbour that is not a wire
//     if that is 15, stop
//     for each of the four horizontals:
//         take the neighbouring wire's power minus one,
//         the one a block up if this side is solid and the ceiling is not,
//         or the one a block down if this side is not solid
//
// The "up" and "down" reaches are what let a wire climb a staircase, and the
// two conditions are not symmetric: a wire climbs *onto* a solid neighbour only
// when nothing solid is directly above itself, and drops *off* a neighbour only
// when that neighbour is not solid. Reversing either makes wire flow through
// ceilings.
//
// The components are simpler and each has one surprise:
//
//   * A **repeater** delays by twice its `delay` property, in ticks, and locks
//     when a powered repeater or comparator points at its side. A locked
//     repeater does not merely stop updating — it stops scheduling, so a change
//     that arrives while it is locked is lost rather than queued.
//
//   * A **comparator** subtracts or compares its back input against the larger
//     of its two sides, and reads a container behind it in place of a signal.
//     Its output does not fit in a block state, so it lives beside it.
//
//   * An **observer** fires on a *state change* in the block it faces, not on
//     movement and not on a signal. It pulses for exactly one tick.
//
//   * A **torch** turns off when the block it is on is powered, and burns out
//     if it is made to change too often — the one place in redstone where the
//     game keeps a history.
#pragma once

#include "ov/gameplay/signal.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"

#include <array>
#include <deque>
#include <vector>

namespace ov::gameplay {

/// How a wire reaches out on one side.
enum class WireSide : u8 { Up, Side, None };

inline constexpr std::array<std::string_view, 3> kWireSideNames{"up", "side", "none"};

/// The four horizontal directions in the order this module iterates them.
inline constexpr std::array<Direction, 4> kHorizontals{Direction::North, Direction::South,
                                                       Direction::West, Direction::East};

/// A torch's burn-out history: eight changes inside sixty ticks and it stops.
///
/// Vanilla keeps this per level, in a list of recent toggles, and drops entries
/// older than the window. The numbers are the ones a real server produces; see
/// docs/provenance/redstone.md.
inline constexpr i32   kTorchBurnoutChanges = 8;
inline constexpr i64   kTorchBurnoutWindow  = 60;
inline constexpr usize kTorchHistoryCap     = 64;

class TorchHistory {
public:
    /// Record a toggle at `pos` on tick `now`, and say whether the torch should
    /// burn out — that is, whether this toggle is the eighth inside the window.
    [[nodiscard]] bool record_and_check(BlockPos pos, i64 now);

    /// Drop entries older than the window. Called once per tick so the list
    /// cannot grow without bound in a world full of clocks.
    void expire(i64 now);

    [[nodiscard]] usize size() const noexcept { return toggles_.size(); }

    void clear() noexcept { toggles_.clear(); }

private:
    struct Toggle {
        BlockPos pos;
        i64      tick;
    };

    std::deque<Toggle> toggles_;
};

/// The redstone rules.
///
/// Stateless apart from the torch history, which is the one thing vanilla
/// itself keeps across ticks.
class Redstone {
public:
    Redstone(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    [[nodiscard]] const Signals& signals() const noexcept { return signals_; }

    [[nodiscard]] TorchHistory& torch_history() noexcept { return torches_; }

    // ── Wire ────────────────────────────────────────────────────────────────

    /// Does a wire at `pos` reach towards `side`, and how?
    [[nodiscard]] WireSide wire_reach(const RedstoneWorld& world, BlockPos pos,
                                      Direction side) const;

    /// The state a wire at `pos` should have, shape and all, given its
    /// surroundings and a power to carry.
    [[nodiscard]] registry::BlockStateId wire_shape(const RedstoneWorld& world, BlockPos pos,
                                                    registry::BlockStateId state) const;

    /// The strength a wire at `pos` should carry.
    ///
    /// This is the one place `wires_signal` is false: while a wire asks what is
    /// around it, every other wire in the world is silent, which is why wire
    /// into a solid block and out again carries nothing.
    [[nodiscard]] i32 wire_strength(const RedstoneWorld& world, BlockPos pos) const;

    /// Recompute a wire and everything its change reaches, writing the results
    /// through `world`.
    ///
    /// Vanilla walks this breadth-first from the changed wire and updates the
    /// neighbours of every wire it rewrites. Doing it one wire at a time
    /// through the ordinary neighbour-update path also terminates, and gives a
    /// different order on a branching circuit.
    void update_wire(RedstoneWorld& world, BlockPos pos);

    // ── Components ──────────────────────────────────────────────────────────

    /// A neighbour of `pos` changed. Returns true if anything was written.
    ///
    /// The single entry point: a block placed, broken or repowered anywhere
    /// calls this for each of its neighbours, and each component decides for
    /// itself whether it cares.
    bool neighbour_changed(RedstoneWorld& world, BlockPos pos, BlockPos from);

    /// A scheduled tick came due at `pos` for `block`.
    bool scheduled_tick(RedstoneWorld& world, BlockPos pos, registry::BlockId block);

    /// The signal a repeater or comparator sees on its input face.
    [[nodiscard]] i32 diode_input(const RedstoneWorld& world, BlockPos pos,
                                  registry::BlockStateId state) const;

    /// The larger of a diode's two side inputs. Only wire, a repeater and a
    /// comparator count sideways — a lever beside a comparator does nothing.
    [[nodiscard]] i32 diode_side_input(const RedstoneWorld& world, BlockPos pos,
                                       registry::BlockStateId state) const;

    /// Is this repeater locked by a powered diode pointing at its side?
    [[nodiscard]] bool repeater_locked(const RedstoneWorld& world, BlockPos pos,
                                       registry::BlockStateId state) const;

    /// What a comparator at `pos` should output.
    [[nodiscard]] i32 comparator_output(const RedstoneWorld& world, BlockPos pos,
                                        registry::BlockStateId state) const;

    /// The comparator reading of a container holding `slots` slots of which the
    /// contents fill `fullness` — the standard 1 + 14 * fill formula, which
    /// gives 15 only for a completely full container and never gives 1 for an
    /// almost-empty one.
    [[nodiscard]] static i32 container_reading(f32 fullness, bool any_items) noexcept;

    /// Does a consumer at `pos` want to be on?
    [[nodiscard]] bool consumer_powered(const RedstoneWorld& world, BlockPos pos) const;

    /// Blocks whose `powered` or `lit` flag simply follows the power around
    /// them, with the delay each of them takes.
    struct ConsumerRule {
        registry::BlockId block;
        std::string_view  flag;
        /// Ticks before the change takes effect, or 0 for immediately.
        i32 delay;
        /// True when the block turns *off* on a delay but on immediately, as
        /// the lamp does.
        bool delay_off_only;
    };

    [[nodiscard]] const ConsumerRule* consumer_rule(registry::BlockId block) const noexcept;

    // ── Named blocks, resolved once ─────────────────────────────────────────

    struct Ids {
        registry::BlockId wire{0};
        registry::BlockId repeater{0};
        registry::BlockId comparator{0};
        registry::BlockId observer{0};
        registry::BlockId torch{0};
        registry::BlockId wall_torch{0};
        registry::BlockId lamp{0};
        registry::BlockId piston{0};
        registry::BlockId sticky_piston{0};
        registry::BlockId piston_head{0};
        registry::BlockId moving_piston{0};
        registry::BlockId dispenser{0};
        registry::BlockId dropper{0};
        registry::BlockId hopper{0};
        registry::BlockId note_block{0};
        registry::BlockId tnt{0};
        registry::BlockId air{0};
    };

    [[nodiscard]] const Ids& ids() const noexcept { return ids_; }

private:
    [[nodiscard]] bool wire_connects_to(const RedstoneWorld& world, BlockPos neighbour,
                                        Direction towards) const;

    bool torch_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state);
    bool repeater_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state);
    bool comparator_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state);
    bool observer_tick(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state);

    const registry::BlockRegistry* blocks_{nullptr};
    Signals                        signals_;
    Ids                            ids_{};
    TorchHistory                   torches_;

    std::vector<ConsumerRule> consumers_;
    /// Indexed by block: an index into `consumers_`, or -1.
    std::vector<i16> consumer_index_;
};

}  // namespace ov::gameplay
