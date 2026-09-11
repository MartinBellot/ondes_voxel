// Rails: the shape a rail takes, the power a powered rail carries, and the
// support it stands on.
//
// Everything here is measured against a real 1.20.1 server by
// scripts/measure_rails.py (docs/provenance/rails-wagonnets.md).
//
//   * **The shape.** A rail set among four neighbour cells — each empty, a rail
//     at the same level, one block up or one block down — for all 256
//     arrangements and six variants (plain or powered centre, set north-south
//     or east-west, beside plain or powered rails). The rule below reproduces
//     all 1536 centres and every neighbour that bent towards them.
//
//     A rail connects to a rail beside it at its own level, one up or one
//     down, when that rail still has a free end. Exactly the two ends it needs
//     are straight or a curve; when more are offered a plain rail takes the
//     **south-east** curve first (then south-west, north-east, north-west),
//     and a straight-only rail keeps the axis it already had. Then a straight
//     rail rises towards a neighbour one block up — south beats north and west
//     beats east when both rise. The neighbours it connects to turn to face it,
//     rising when it is above them.
//
//   * **Power.** A powered or activator rail is powered by a neighbour signal,
//     or by a rail of its own kind up to eight rails along its line that is
//     itself powered and receives a signal: 17 rails for one source in the
//     middle of a line (the wiki's number, measured in `power`).
//
// Layer 9: everything takes a `RedstoneWorld&`, which is a `LevelWriter`,
// never a server — a client could run the same rule and predict it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/signal.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace ov::gameplay {

/// The ten values of `shape`, in the order the registry declares them. The
/// three straight-only rails have the first six.
enum class RailShape : u8 {
    NorthSouth,
    EastWest,
    AscendingEast,
    AscendingWest,
    AscendingNorth,
    AscendingSouth,
    SouthEast,
    SouthWest,
    NorthWest,
    NorthEast,
};

inline constexpr std::array<std::string_view, 10> kRailShapeNames{
    "north_south",    "east_west",       "ascending_east", "ascending_west", "ascending_north",
    "ascending_south", "south_east",     "south_west",     "north_west",     "north_east",
};

[[nodiscard]] constexpr bool is_ascending(RailShape shape) noexcept {
    return shape == RailShape::AscendingEast || shape == RailShape::AscendingWest ||
           shape == RailShape::AscendingNorth || shape == RailShape::AscendingSouth;
}

/// Which of the four rails a block is.
enum class RailKind : u8 { None, Plain, Powered, Detector, Activator };

/// One end of a rail: the horizontal step it leads to, and whether that end
/// is the raised one of a slope.
struct RailEnd {
    i32  dx{0};
    i32  dz{0};
    bool rises{false};
};

struct RailEnds {
    RailEnd first;
    RailEnd second;
};

/// The two ends of a shape. For a slope the raised end is the one named by
/// the shape: `ascending_east` rises towards +x.
[[nodiscard]] RailEnds rail_ends(RailShape shape) noexcept;

/// What a neighbour change did to a rail, for the caller to finish: a rail
/// that lost its support drops as an item, and a rail whose power changed
/// has to wake the blocks beside the one under it (and above it, on a slope),
/// which is how the change travels up and down a sloped line.
struct RailUpdate {
    bool                   broke{false};
    /// The state that broke, for its loot.
    registry::BlockStateId was{};
    bool                   power_changed{false};
    bool                   reshaped{false};
};

class Rails {
public:
    Rails(const registry::BlockRegistry& blocks, const Signals& signals);

    [[nodiscard]] RailKind kind_of(registry::BlockStateId state) const noexcept;
    [[nodiscard]] RailKind kind_of_block(registry::BlockId block) const noexcept;
    [[nodiscard]] bool     is_rail(registry::BlockStateId state) const noexcept {
        return kind_of(state) != RailKind::None;
    }
    /// Powered, detector and activator rails cannot curve.
    [[nodiscard]] static constexpr bool straight_only(RailKind kind) noexcept {
        return kind != RailKind::Plain && kind != RailKind::None;
    }

    [[nodiscard]] std::optional<RailShape> shape_of(registry::BlockStateId state) const noexcept;
    [[nodiscard]] registry::BlockStateId   with_shape(registry::BlockStateId state,
                                                      RailShape              shape) const noexcept;
    [[nodiscard]] bool                     powered(registry::BlockStateId state) const noexcept;
    [[nodiscard]] registry::BlockStateId   with_powered(registry::BlockStateId state,
                                                        bool                   on) const noexcept;
    [[nodiscard]] bool                     waterlogged(registry::BlockStateId state) const noexcept;

    /// The state a player's placement starts from, before `on_placed` shapes
    /// it: straight along the axis the player faces (the wiki: "orients itself
    /// in the direction the player is facing"; not measured here), and
    /// waterlogged when set into a water source.
    [[nodiscard]] registry::BlockStateId placement_state(const world::LevelView& level,
                                                         BlockPos pos, registry::BlockStateId state,
                                                         bool facing_east_west) const noexcept;

    /// Does a state offer a rail the rim it needs on top? Any full block, a
    /// hopper, a top slab: the face must cover its outer two-sixteenths ring.
    [[nodiscard]] bool rigid_top(registry::BlockStateId state) const noexcept;

    /// Can the rail at `pos` stand: a rim under it and, for a slope, under
    /// the end it rises to?
    [[nodiscard]] bool supported(const world::LevelView& level, BlockPos pos,
                                 registry::BlockStateId state) const noexcept;

    /// A rail has just been set at `pos`. Pick its shape among its neighbours,
    /// write it, and bend the neighbours it connects to towards it.
    ///
    /// Vanilla does this in the rail's own placement, which is why `/setblock`
    /// shapes a rail too; a neighbour change never reshapes one (except the
    /// T-junction switch below).
    void on_placed(RedstoneWorld& world, BlockPos pos) const;

    /// Something next to the rail at `pos` changed: its support, its power,
    /// or — for a plain rail at a T-junction beside a redstone component — its
    /// curve, which a signal turns the other way (the wiki's "redstone
    /// switching": north-west first when powered).
    RailUpdate neighbour_changed(RedstoneWorld& world, BlockPos pos) const;

    /// Should the powered or activator rail at `pos` be powered?
    [[nodiscard]] bool wants_power(const RedstoneWorld& world, BlockPos pos,
                                   registry::BlockStateId state) const;

    /// The shape a rail at `pos` would take among its current neighbours.
    /// `powered` inverts a plain rail's preference at a junction.
    [[nodiscard]] RailShape choose_shape(const RedstoneWorld& world, BlockPos pos,
                                         registry::BlockStateId state, bool powered) const;

    /// Set a detector rail's `powered`. Returns true if it changed.
    bool set_detector(RedstoneWorld& world, BlockPos pos, bool occupied) const;

    /// Is this a redstone conductor? A powered rail launches a cart at rest
    /// away from one at either end.
    [[nodiscard]] bool conductor(registry::BlockStateId state) const noexcept {
        return signals_->is_conductor(state);
    }

    /// Search length, in rails, from one directly powered rail.
    static constexpr i32 kPowerReach = 8;

private:
    struct Found {
        bool     present{false};
        BlockPos pos{};
    };

    [[nodiscard]] Found find_rail(const world::LevelView& level, BlockPos pos, i32 dx,
                                  i32 dz) const noexcept;
    [[nodiscard]] i32   connection_count(const world::LevelView& level, BlockPos pos,
                                         registry::BlockStateId state) const noexcept;
    [[nodiscard]] bool  connects_towards(const world::LevelView& level, BlockPos pos,
                                         registry::BlockStateId state, i32 dx,
                                         i32 dz) const noexcept;
    [[nodiscard]] bool  can_take(const world::LevelView& level, BlockPos neighbour, i32 dx_to_us,
                                 i32 dz_to_us) const noexcept;
    void                connect_to(RedstoneWorld& world, BlockPos neighbour, BlockPos us) const;
    void                apply_shape(RedstoneWorld& world, BlockPos pos, registry::BlockStateId state,
                                    RailShape shape, bool force_write) const;
    [[nodiscard]] bool  line_powered(const RedstoneWorld& world, BlockPos pos, RailKind kind,
                                     i32 dx, i32 dz, i32 depth) const;
    [[nodiscard]] bool  beside_component(const RedstoneWorld& world, BlockPos pos) const;

    const registry::BlockRegistry* blocks_;
    const Signals*                 signals_;

    registry::BlockId plain_{0};
    registry::BlockId powered_{0};
    registry::BlockId detector_{0};
    registry::BlockId activator_{0};
    registry::BlockId water_{0};
};

}  // namespace ov::gameplay
