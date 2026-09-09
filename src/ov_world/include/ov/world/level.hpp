// The world as a behaviour sees it.
//
// A block behaviour — a fluid spreading, a redstone wire updating, a sapling
// growing — needs to read blocks around it, write a few, and ask to be woken
// later. It must not need the server: the client predicts the same behaviours
// against its own replica, and a rule written against `ServerLevel&` can only
// ever run on one side. So the rules take one of these two references and the
// two worlds each supply their own implementation.
//
// This is why the interface is deliberately small. Everything on it is
// something a *client replica* can also answer. Nothing here exposes entities,
// players, the tick loop or the network — a fluid that needed any of those
// would be telling us the rule had been written the wrong way round.
//
// Virtual dispatch is the right cost here. These are called a few hundred
// times per tick, not per block per frame; the alternative — templating the
// rules on the world type — would put the world's storage into every public
// gameplay header, which the module rules forbid outright.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/world/block_ticks.hpp"
#include "ov/world/chunk.hpp"

#include <string_view>

namespace ov::world {

/// What a dimension does to the rules that run in it.
///
/// Carried on the level rather than looked up from a name: a datapack
/// dimension has its own answers, and gameplay has no business holding a table
/// of dimension names.
struct DimensionTraits {
    /// The Nether. Lava flows as far and as fast as water does here — measured:
    /// a lava source on flat netherrack makes a diamond of radius 7 with levels
    /// 0..7, against radius 3 with levels 0,2,4,6 in the Overworld.
    bool ultrawarm{false};

    /// The Nether and the End: no rain, and water evaporates.
    bool natural{true};
};

/// Reading the world.
class LevelView {
public:
    LevelView()                            = default;
    LevelView(const LevelView&)            = delete;
    LevelView& operator=(const LevelView&) = delete;
    LevelView(LevelView&&)                 = delete;
    LevelView& operator=(LevelView&&)      = delete;
    virtual ~LevelView()                   = default;

    /// The state at a position.
    ///
    /// Outside the world's vertical extent, and in a chunk that is not loaded,
    /// this returns air. That is not a silent default: a behaviour that needs
    /// to tell "air" from "not loaded" asks `is_loaded` — and fluids do,
    /// because flowing into an unloaded chunk is how a world generates itself
    /// differently depending on where the player walked.
    [[nodiscard]] virtual registry::BlockStateId block_at(BlockPos pos) const = 0;

    [[nodiscard]] virtual bool is_loaded(BlockPos pos) const = 0;

    [[nodiscard]] virtual WorldShape shape() const = 0;

    [[nodiscard]] virtual DimensionTraits traits() const = 0;

    /// The registry the states above are numbered by.
    [[nodiscard]] virtual const registry::BlockRegistry& blocks() const = 0;
};

/// Reading and changing the world.
class LevelWriter : public LevelView {
public:
    /// Place a state.
    ///
    /// Implementations are expected to do whatever the world needs around a
    /// change — heightmaps, light, telling clients. A behaviour never does that
    /// itself, which is exactly what lets the same behaviour run on a replica
    /// that does none of it.
    virtual void set_block(BlockPos pos, registry::BlockStateId state) = 0;

    /// Ask to be woken at this position in `delay` ticks.
    ///
    /// `what` is the registry name the tick will be recognised by, and which
    /// queue it lands in follows from `queue`. See block_ticks.hpp for why the
    /// two queues name things from two different registries.
    virtual void schedule_tick(BlockPos pos, std::string_view what, i64 delay, TickQueue queue,
                               TickPriority priority = TickPriority::Normal) = 0;

    [[nodiscard]] virtual bool has_scheduled_tick(BlockPos pos, std::string_view what,
                                                  TickQueue queue) const = 0;

    /// The tick the world is on. Behaviours use it only to compute delays.
    [[nodiscard]] virtual i64 game_time() const = 0;
};

}  // namespace ov::world
