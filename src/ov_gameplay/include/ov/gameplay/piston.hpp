// Pistons: what moves, what refuses, and the bug the game kept.
//
// Three rules, and one of them is a bug that parity requires.
//
//   * **Twelve blocks and no more.** A piston pushes a column of at most twelve;
//     the thirteenth makes the whole push fail rather than pushing twelve of
//     them. Slime and honey drag their neighbours sideways into the same
//     twelve, which is why a machine that works with eleven blocks stops dead
//     when someone adds a decorative one.
//
//   * **Some blocks cannot be pushed at all**, some break instead of moving,
//     and the difference is per block: obsidian refuses, a torch breaks, a
//     block entity refuses. Refusing is not the same as breaking — one stops
//     the piston, the other lets it through and destroys something.
//
//   * **Quasi-connectivity.** A piston is powered by anything that powers the
//     block *above* it, even when that space is air and the source shares no
//     face with the piston. It has been in the game since 2011, every redstone
//     machine built since assumes it, and it is not going to be fixed.
//
//     It is not a free-floating rule: it is checked when the piston is told to
//     look, and `setblock` tells only six neighbours. A source dropped two
//     blocks from a standing piston changes nothing until something else nudges
//     it — which is why the same arrangement is called a block update detector.
//     Both halves were measured; see docs/provenance/redstone.md.
#pragma once

#include "ov/gameplay/redstone.hpp"
#include "ov/math/block_pos.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The most blocks one piston can move.
inline constexpr usize kPistonPushLimit = 12;

/// What happens to a block a piston tries to move.
enum class PushReaction : u8 {
    /// Moves, and takes sticky neighbours with it.
    Normal,
    /// Refuses to move and stops the whole push.
    Block,
    /// Breaks instead of moving.
    Destroy,
    /// Moves but never drags anything: slime does not stick to it.
    PushOnly,
    /// Not there at all — air, a fluid, a replaceable plant.
    Ignore,
};

/// One block on its way somewhere.
struct MovingBlock {
    BlockPos               from;
    registry::BlockStateId state;
};

/// The outcome of asking a piston to move.
struct PushPlan {
    bool                     possible{false};
    std::vector<MovingBlock> moved;
    /// Blocks the push destroys rather than moves, in the order vanilla breaks
    /// them: furthest first, so a dropped item cannot land in a space that is
    /// about to be filled.
    std::vector<MovingBlock> destroyed;
    /// Why it failed, for the caller that wants to say so rather than shrug.
    enum class Refusal : u8 { None, TooMany, Immovable, OutOfWorld } refusal{Refusal::None};
};

class Pistons {
public:
    Pistons(const registry::BlockRegistry& blocks, const registry::Registries& registries,
            const Redstone& redstone);

    [[nodiscard]] PushReaction reaction_of(registry::BlockStateId state) const noexcept;

    /// Is this piston being asked to extend?
    ///
    /// The quasi-connectivity check lives here and nowhere else: a piston looks
    /// at its own six neighbours, skipping the face it points at, and then at
    /// the six neighbours of the block above it, skipping the one below — which
    /// is the piston itself, and would otherwise make every extended piston
    /// hold itself out.
    [[nodiscard]] bool wants_extended(const RedstoneWorld& world, BlockPos pos,
                                      Direction facing) const;

    /// Work out what a push would move, without moving anything.
    [[nodiscard]] PushPlan plan_push(const RedstoneWorld& world, BlockPos piston,
                                     Direction facing, bool extending) const;

    /// A sticky piston retracting: the single block stuck to its head, if any.
    [[nodiscard]] PushPlan plan_pull(const RedstoneWorld& world, BlockPos piston,
                                     Direction facing) const;

    /// Handle a neighbour change at a piston. Returns true if it wrote.
    bool neighbour_changed(RedstoneWorld& world, BlockPos pos);

    /// A piston's scheduled tick came due: finish the extension or retraction.
    bool scheduled_tick(RedstoneWorld& world, BlockPos pos, registry::BlockId block);

    /// The same, taking the name the scheduler hands back.
    bool scheduled_tick(RedstoneWorld& world, BlockPos pos, std::string_view what);

    /// Does this state stick to its neighbours?
    [[nodiscard]] bool is_sticky_block(registry::BlockStateId state) const noexcept;

    /// Blocks whose push reaction was not read from the game, and is inferred
    /// from the collision shape instead.
    ///
    /// Named rather than hidden: a caller that wants to know how far the
    /// measurement reaches can ask, and a test can assert the list has not
    /// grown.
    [[nodiscard]] std::span<const std::string_view> unmeasured() const noexcept {
        return unmeasured_;
    }

private:
    /// Gather the column a push moves, including everything slime drags along.
    [[nodiscard]] bool gather(const RedstoneWorld& world, BlockPos start, Direction facing,
                              PushPlan& plan) const;

    const registry::BlockRegistry* blocks_{nullptr};
    const Redstone*                redstone_{nullptr};

    std::vector<PushReaction>     reactions_;
    std::vector<std::string_view> unmeasured_;
    registry::BlockId         slime_{0};
    registry::BlockId         honey_{0};
};

}  // namespace ov::gameplay
