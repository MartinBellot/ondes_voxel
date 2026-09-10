// Blocks that fall, and the entity they fall as.
//
// Sixteen colours of concrete powder, sand, red sand, gravel, three anvils,
// the dragon egg and the two suspicious blocks are held up by nothing but the
// block under them. When that block goes, they become an entity — a
// `minecraft:falling_block` carrying their state — that drops under gravity
// and turns back into a block where it lands.
//
// Everything with a number was measured on a real 1.20.1 server by
// scripts/measure_tnt_gravity.py sand (docs/provenance/tnt-et-gravite.md):
//
//   * **Two ticks.** A sand block set in mid-air starts falling two ticks
//     later, and in a column each block starts two ticks after the one under
//     it — because the one under it becoming air is the neighbour change that
//     schedules it. Four columns of six, all six on the same stagger.
//
//   * **0.04 and 0.98, drag after the move.** The falling entity's `Motion`
//     tag reads -0.0392 after one tick, -0.077616 after two: gravity, then the
//     move, then ×0.98 — exactly the primed TNT's numbers.
//
//   * **What it lands in.** A cell that holds something replaceable (the
//     `#minecraft:replaceable` tag: air, water, grass, snow…) takes the block.
//     Anything else — a torch — keeps its place and the falling block drops as
//     one item.
//
//   * **Sand sinks, powder stops.** Sand dropped into a pool four deep went to
//     the floor; red concrete powder dropped the same way hardened in the
//     **top** water cell. See `lands_in_water`.
//
//   * **Five faces of six.** Powder set beside water hardens when the water is
//     above it or on any of its four sides. With the water **below** it does
//     not harden in place: it falls into it.
//
// Nothing is re-evaluated when a chunk loads — vanilla does not either, and a
// sand bridge built with /setblock stays up until something next to it
// changes. That is why this module is only ever woken by a neighbour change or
// by its own scheduled tick.
#pragma once

#include "ov/entity/logic.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The motion of the two gravity-driven block entities, primed TNT and falling
/// blocks. One struct so a trace can be fitted against it one field at a time.
struct BlockEntityMotion {
    /// Subtracted from the vertical velocity each tick, before the move.
    f64 gravity{0.04};
    /// Every axis is multiplied by this after the move. Not 0.98 on y and 0.91
    /// on x and z as for a mob: the measured horizontal speed of a primed TNT
    /// in the air is `0.02 × 0.98ⁿ` to seventeen decimals.
    f64 drag{0.98};
    /// Horizontal, on top of `drag`, on a tick that ends on the ground.
    /// Measured: 0.01701526 in the air, 0.01167247 on the first grounded tick,
    /// a ratio of 0.686 = 0.98 × 0.7.
    f64 ground_horizontal{0.7};
};

/// Advance a TNT or falling block by one tick of gravity, collision and drag.
///
/// Returns the new state. Unlike `step_entity`, a component below 0.003 is
/// **not** rounded to zero: a grounded TNT's horizontal speed was measured at
/// 0.00177 and still shrinking, which a mob's threshold would have erased.
/// A vertical collision stops the vertical velocity outright — measured, the
/// Motion of a TNT resting on stone reads exactly 0.0, not a bounce.
[[nodiscard]] entity::EntityState step_block_entity(const entity::EntityState& state,
                                                    const BlockEntityMotion&   motion,
                                                    const CollisionWorld&      world,
                                                    bool                       gravity = true);

/// The delay between a falling block learning its support is gone and falling.
inline constexpr i32 kFallDelayTicks = 2;

/// A falling block that has gone this long without landing drops as an item.
/// From the wiki's Falling Block article; not measured here.
inline constexpr i32 kFallingBlockMaxTicks = 600;

/// What the level asked the caller to create: a falling entity at a block.
struct FallStart {
    BlockPos               pos{};
    registry::BlockStateId state{};
};

/// What happened where a falling block came down.
enum class Landing : u8 {
    /// It became a block in the cell.
    Placed,
    /// The cell would not take it, and it breaks into its item.
    Dropped,
    /// It breaks and gives nothing: a suspicious block's loot is its brush
    /// reward, and vanilla drops nothing when one falls onto a torch.
    Vanished,
};

/// The rules, with the blocks they concern resolved once.
class FallingBlocks {
public:
    FallingBlocks(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// Does this state fall when unsupported?
    [[nodiscard]] bool falls(registry::BlockStateId state) const noexcept;

    /// May a falling block fall into this state? Air, fire, a fluid, or
    /// anything replaceable.
    [[nodiscard]] bool is_free(registry::BlockStateId state) const noexcept;

    /// May a falling block that lands here replace it?
    [[nodiscard]] bool replaceable(registry::BlockStateId state) const noexcept;

    /// Is this a concrete powder, and what does it harden into?
    [[nodiscard]] std::optional<registry::BlockStateId> hardened(
        registry::BlockStateId state) const noexcept;

    /// Is there water on one of the five faces that harden powder in place?
    [[nodiscard]] bool touches_water(const world::LevelView& level, BlockPos pos) const;

    /// Is this state water — a source, a flow, or a waterlogged block?
    [[nodiscard]] bool is_water(registry::BlockStateId state) const noexcept;

    /// A neighbour of `pos` changed, or the block at `pos` was just written.
    ///
    /// Two answers, both in the game: powder beside water hardens on the spot,
    /// and a block with nothing under it asks to be woken in two ticks. Returns
    /// true when anything was written or scheduled.
    bool neighbour_changed(world::LevelWriter& level, BlockPos pos) const;

    /// A scheduled tick came due. Returns the entity to create, having already
    /// emptied the cell, or nothing when the block is supported again or is
    /// not a falling block at all.
    [[nodiscard]] std::optional<FallStart> tick(world::LevelWriter& level, BlockPos pos,
                                                std::string_view what) const;

    /// Put a landed block down, or refuse. `in_water` is a powder that
    /// stopped because it entered water: it hardens where it is.
    Landing land(world::LevelWriter& level, BlockPos cell, registry::BlockStateId state,
                 bool in_water) const;

    /// Does a falling block of this state stop in water rather than sink?
    [[nodiscard]] bool lands_in_water(registry::BlockStateId state) const noexcept;

    /// The block a falling block turns into as an item. The block itself for
    /// every falling block this module knows.
    [[nodiscard]] std::string_view item_of(registry::BlockStateId state) const;

    /// Falling blocks this server recognises and does not fully carry out,
    /// named rather than silently approximated.
    [[nodiscard]] static std::span<const std::string_view> unimplemented() noexcept;

private:
    const registry::BlockRegistry* blocks_{nullptr};
    std::vector<u8>                falls_;        // by block id
    std::vector<u8>                free_;         // by block id
    std::vector<u8>                replaceable_;  // by block id
    std::vector<registry::BlockStateId> hardened_; // by block id; 0 = not a powder
    std::vector<u8>                vanishes_;     // by block id
    registry::BlockId              water_{0};
    registry::BlockId              snow_{0};
};

/// Where a falling block's logic reports what it did.
///
/// A queue rather than a call back into the world: the entity world is ticking
/// when this happens, and the caller acts on the list once the tick is over.
struct FallingEvents {
    struct Landed {
        i32                    network_id{0};
        BlockPos               cell{};
        registry::BlockStateId state{};
        bool                   in_water{false};
        /// Time ran out rather than ground being reached: it drops as an item.
        bool                   expired{false};
    };
    std::vector<Landed> landed;
};

/// The logic of one `minecraft:falling_block`.
class FallingBlockLogic final : public entity::IEntityLogic {
public:
    FallingBlockLogic(const FallingBlocks& rules, registry::BlockStateId state, BlockPos start,
                      FallingEvents& events, BlockEntityMotion motion = {}) noexcept
        : rules_{&rules}, state_{state}, start_{start}, events_{&events}, motion_{motion} {}

    void tick(entity::EntityWorld& world, entity::EntityHandle self,
              const entity::TickContext& context) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "falling_block"; }

    [[nodiscard]] registry::BlockStateId state() const noexcept { return state_; }
    [[nodiscard]] BlockPos               start() const noexcept { return start_; }
    [[nodiscard]] i32                    time() const noexcept { return time_; }

private:
    const FallingBlocks*   rules_;
    registry::BlockStateId state_;
    BlockPos               start_;
    FallingEvents*         events_;
    BlockEntityMotion      motion_;
    i32                    time_{0};
};

}  // namespace ov::gameplay
