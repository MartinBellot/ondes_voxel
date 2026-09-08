// The blocks that reshape themselves around their neighbours.
//
// A fence, a pane and a wall each decide, per side, whether to reach out. The
// rule is the same shape for all three:
//
//     attach = (the neighbour is not an exception and its facing face is full)
//              or the neighbour belongs to the same family
//
// The "full face" half comes from the collision shapes, which are per state:
// a bottom slab fills the floor's face and nothing else, so the same block
// answers three different ways. That is why there is no per-block table here.
//
// The exception list is measured, not read from anywhere: a fence was placed
// against every block state on a real 1.20.1 server, and the blocks that offer
// a full face and are still refused are exactly the ones below. `target` is in
// that list, which no amount of reasoning would have produced.
#pragma once

#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The four horizontal directions, in the order the state properties name them.
enum class Side : u8 { North, South, West, East };

inline constexpr std::array<std::string_view, 4> kSideNames{"north", "south", "west", "east"};

/// Which family a block belongs to, as far as reaching out goes.
enum class ConnectingKind : u8 {
    /// Reshapes for nothing.
    None,
    /// Wooden fences, and the nether brick one which keeps to itself.
    Fence,
    NetherBrickFence,
    /// Panes and iron bars, which behave alike.
    Pane,
    /// A gate is not itself reshaped, but a fence reaches for one.
    FenceGate,
    /// Walls have three values a side rather than two; they are recognised but
    /// not yet reshaped, because `low` and `tall` need what is above.
    Wall,
    /// Stairs take one of five shapes depending on the stair in front of or
    /// behind them.
    Stairs,
};

class Connections {
public:
    Connections(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    [[nodiscard]] ConnectingKind kind_of(registry::BlockId block) const noexcept;

    /// Does a block of `kind` reach towards this neighbour on this side?
    [[nodiscard]] bool attaches(ConnectingKind kind, Side side,
                                registry::BlockStateId neighbour) const noexcept;

    /// The state this one takes given its four horizontal neighbours.
    ///
    /// Returns the state unchanged for anything that does not reshape, so a
    /// caller can run it over every placement without asking first.
    [[nodiscard]] registry::BlockStateId reshape(
        registry::BlockStateId                       state,
        const std::array<registry::BlockStateId, 4>& around) const noexcept;

    /// The shape a stair takes given its four horizontal neighbours.
    ///
    /// Five shapes, and the rule was measured exhaustively — 256 pairs, then a
    /// third pass for the block that keeps a corner from forming. A corner
    /// needs a stair in front of or behind, on the same half, facing across;
    /// and exactly one of the two crossing directions cancels it.
    [[nodiscard]] registry::BlockStateId stair_shape(
        registry::BlockStateId                       state,
        const std::array<registry::BlockStateId, 4>& around) const noexcept;

private:
    const registry::BlockRegistry* blocks_{nullptr};

    /// Per block: its family, whether a full face is enough to attach to it,
    /// and whether its support face counts as full even when its collision box
    /// is not.
    std::vector<ConnectingKind> kinds_;
    std::vector<bool>           refuses_;
    std::vector<bool>           support_full_;
};

}  // namespace ov::gameplay
