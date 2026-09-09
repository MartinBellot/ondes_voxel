// Fluids: water and lava as blocks that flow.
//
// The project already knew how to *swim* — the drag and the buoyancy a player
// feels in water were measured long before this file existed. What was missing
// is the other half: a bucket that empties makes something spread, a dam that
// breaks drowns what is behind it.
//
// Everything below was measured against a real 1.20.1 server: blocks placed by
// the console, the world left to settle, then read back out of the Anvil save.
// See docs/provenance/fluides.md for the scenarios and the counts. The results
// worth stating up front, because none of them is guessable and three of them
// contradict the obvious guess:
//
//   * **Water thins by one per block and stops at 7.** A source dropped on a
//     flat floor makes a diamond of radius 7 whose level is the Manhattan
//     distance — 221 positions, all of them.
//   * **Lava thins by two in the Overworld and by one in the Nether.** So the
//     same source reaches 3 blocks with levels 0,2,4,6 on stone and 7 blocks
//     with levels 0..7 on netherrack. The Nether puddle is indistinguishable
//     from water's.
//   * **Water searches for a hole and only flows towards it.** Not a metaphor:
//     with a hole four blocks east, the source makes a one-block-wide line east
//     and nothing else — the other three directions stay dry. The search is a
//     breadth-first walk of the horizontal plane out to **exactly 5** (a hole
//     at 5 is found, one at 6 is not, and at 6 the water goes back to making a
//     disc), it goes **around obstacles**, and **every** direction tied for the
//     shortest path receives flow.
//   * **Two horizontally adjacent water sources make a third.** Lava does not.
//   * **A falling fluid feeds sideways at full strength.** Water landing at the
//     bottom of a shaft spreads with level 1 neighbours, as a source would.
//   * The lava/water table is asymmetric and depends on geometry:
//     water reaching a lava **source** makes obsidian, water reaching **flowing**
//     lava makes cobblestone, and lava sitting **above** water turns the
//     **water** into stone. One scenario shows all three at once: water poured
//     onto a settled lava pool leaves an obsidian centre inside a cobblestone
//     ring.
//   * **Flowing water never waterlogs anything.** A slab in the path of a flow
//     stays dry and the water goes around it. Waterlogging is something a
//     *placement* does, which is why it lives on `waterlog` here and not in the
//     spreading rule.
#pragma once

#include "ov/registry/block_states.hpp"
#include "ov/world/level.hpp"

#include <array>
#include <optional>
#include <vector>

namespace ov::gameplay {

/// Which fluid, if any.
enum class FluidKind : u8 { None, Water, Lava };

/// The fluid occupying one position.
///
/// `amount` is vanilla's own scale and runs the way that reads backwards: **0
/// is a full source** and 7 is the thinnest film. That is what the `level`
/// block state property stores, so flipping it here would mean flipping it
/// again at every read and write of the world.
struct FluidState {
    FluidKind kind{FluidKind::None};
    u8        amount{0};
    /// Fluid with nothing holding it up. Stored on disk as `level=8`, whatever
    /// the amount, and it feeds its horizontal neighbours as a source would.
    bool falling{false};

    [[nodiscard]] constexpr bool empty() const noexcept { return kind == FluidKind::None; }

    [[nodiscard]] constexpr bool is_source() const noexcept {
        return kind != FluidKind::None && amount == 0 && !falling;
    }

    /// How full this is, 0 for nothing and 8 for a source. The number
    /// comparisons want, since `amount` runs the other way.
    [[nodiscard]] constexpr u8 height() const noexcept {
        return kind == FluidKind::None ? 0 : static_cast<u8>(8 - amount);
    }

    friend constexpr bool operator==(const FluidState&, const FluidState&) noexcept = default;
};

/// The horizontal directions, in the order the search visits them.
inline constexpr std::array<Direction, 4> kHorizontal{Direction::North, Direction::South,
                                                      Direction::West, Direction::East};

/// The measured constants, named rather than spelled inline.
struct FluidConstants {
    /// Levels lost per block travelled. Measured: 1 for water everywhere, 1 for
    /// lava in the Nether, 2 for lava in the Overworld.
    static constexpr u8 kWaterDropOff       = 1;
    static constexpr u8 kLavaDropOffNormal  = 2;
    static constexpr u8 kLavaDropOffNether  = 1;
    /// The thinnest fluid there is; past this a block is dry.
    static constexpr u8 kMinAmount = 7;

    /// Ticks between one spread and the next. Read straight out of the `t`
    /// field a real server wrote for a freshly placed source, so these are
    /// exact rather than timed.
    static constexpr i32 kWaterDelay       = 5;
    static constexpr i32 kLavaDelayNormal  = 30;
    static constexpr i32 kLavaDelayNether  = 10;

    /// How far the hole search looks. Measured to be exactly 5: a hole at 5 is
    /// found and one at 6 is not.
    static constexpr i32 kSearchRadius = 5;

    /// How far a sponge reaches, and how much it may take.
    static constexpr i32   kSpongeRadius = 7;
    static constexpr usize kSpongeMax    = 65;
};

/// The fluid rules, with the registry lookups they need resolved once.
///
/// Built per level rather than per tick: every call would otherwise look up
/// water, lava and the `level` property by name, and a flowing pool does that
/// thousands of times a second.
class FluidRules {
public:
    explicit FluidRules(const registry::BlockRegistry& blocks);

    /// The fluid a state holds — the block itself, or its waterlogging.
    ///
    /// A waterlogged stair answers with a water **source**, which is the whole
    /// reason waterlogging is visible to this file: measured on a real server,
    /// a waterlogged slab alone on a floor feeds its four neighbours at level 1
    /// and grows the same diamond a source would.
    [[nodiscard]] FluidState fluid_at(registry::BlockStateId state) const noexcept;

    /// The state a fluid becomes as a block of its own.
    [[nodiscard]] registry::BlockStateId state_for(FluidState fluid) const noexcept;

    /// Ticks between spreads, for this fluid in this dimension.
    [[nodiscard]] i32 tick_delay(FluidKind kind, world::DimensionTraits traits) const noexcept;

    /// Levels lost per block, for this fluid in this dimension.
    [[nodiscard]] u8 drop_off(FluidKind kind, world::DimensionTraits traits) const noexcept;

    /// The name a scheduled fluid tick carries on disk.
    ///
    /// Not the block's name: Anvil's `fluid_ticks` name the **fluid**, and the
    /// fluid registry tells a source from a flow — `minecraft:water` against
    /// `minecraft:flowing_water`. Both appear in a save from a real server.
    [[nodiscard]] static std::string_view tick_name(FluidState fluid) noexcept;

    // ── The rules ───────────────────────────────────────────────────────────

    /// Run one scheduled tick of the fluid at `pos`.
    ///
    /// This is the whole of the spreading rule: work out what this position
    /// should now hold given its neighbours, then feed whatever it can below
    /// and beside it.
    void tick(world::LevelWriter& level, BlockPos pos) const;

    /// A block next to `pos` changed. Wake the fluid if there is one.
    void on_neighbour_changed(world::LevelWriter& level, BlockPos pos) const;

    /// Put a fluid into the world the way a bucket does, and start it flowing.
    ///
    /// Handles waterlogging: emptying a water bucket into a slab, a stair, a
    /// fence or a sign fills it rather than replacing it. Returns false when
    /// the position takes neither.
    bool place_fluid(world::LevelWriter& level, BlockPos pos, FluidKind kind) const;

    /// Take the fluid back out, as a bucket does. Returns what was picked up.
    ///
    /// Only a source can be picked up, which is why this returns nothing over a
    /// flow. A waterlogged block is drained and kept.
    [[nodiscard]] std::optional<FluidKind> pick_up_fluid(world::LevelWriter& level,
                                                         BlockPos            pos) const;

    /// The state a block should take when it is placed in fluid, or nothing if
    /// it does not waterlog. Used by placement, not by the flow.
    [[nodiscard]] std::optional<registry::BlockStateId> waterlogged_variant(
        registry::BlockStateId state, bool waterlogged) const noexcept;

    /// Does breaking this block release water? Answered from the state, so a
    /// dry slab and a waterlogged one differ.
    [[nodiscard]] bool releases_water(registry::BlockStateId state) const noexcept;

    /// The state left behind when this block is broken, which is water rather
    /// than air if it was waterlogged.
    [[nodiscard]] registry::BlockStateId state_after_break(
        registry::BlockStateId state) const noexcept;

    /// Which way the fluid at `pos` pushes an entity standing in it.
    ///
    /// The sum, over the four horizontal neighbours, of the drop in fluid
    /// height — so a still pool pushes nothing and a flow pushes downhill. Not
    /// normalised: the caller scales it by the fluid's own strength.
    [[nodiscard]] Vec3d flow_vector(const world::LevelView& level, BlockPos pos) const;

    /// Run a sponge placed at `pos`: dry everything within reach.
    ///
    /// Returns how many blocks were taken, which is what tells the caller
    /// whether the sponge should turn wet. Measured: a sponge dropped into a
    /// settled puddle absorbs it whole and becomes `minecraft:wet_sponge`.
    usize absorb(world::LevelWriter& level, BlockPos pos) const;

    // ── Exposed for testing ─────────────────────────────────────────────────

    /// The horizontal directions the fluid at `pos` would spread into.
    ///
    /// This is the hole search: a breadth-first walk out to radius 5 that
    /// returns every direction tied for the shortest path to somewhere the
    /// fluid can fall. When nothing within reach falls away, every direction it
    /// can enter is returned and the puddle spreads as a disc.
    void spread_directions(const world::LevelView& level, BlockPos pos, FluidState fluid,
                           std::array<bool, 4>& out) const;

    /// What `pos` should hold, from its neighbours alone.
    ///
    /// Ignores whatever is there now, which is what makes a fluid recover from
    /// a state nothing would have produced — a chunk edited by hand, or a
    /// source removed under a flow.
    [[nodiscard]] FluidState computed_state(const world::LevelView& level, BlockPos pos) const;

private:
    /// Can fluid of this kind occupy this state at all?
    [[nodiscard]] bool can_hold(registry::BlockStateId state, FluidKind kind) const noexcept;

    /// Can fluid pass from `from` through the face between it and `to`?
    [[nodiscard]] bool can_flow_through(const world::LevelView& level, BlockPos from, BlockPos to,
                                        Direction direction, FluidKind kind) const;

    /// Apply the lava/water table at `pos`, where fluid of `arriving` is about
    /// to enter. Returns true when a block was made and the flow stops there.
    bool try_mix(world::LevelWriter& level, BlockPos pos, FluidKind arriving) const;

    /// Turn whatever lava is at `pos` into its meeting-water product, and say
    /// whether there was any.
    bool harden_lava(world::LevelWriter& level, BlockPos pos) const;

    /// Would two horizontally adjacent sources make this one a source too?
    [[nodiscard]] bool becomes_source(const world::LevelView& level, BlockPos pos,
                                      FluidKind kind) const;

    void spread(world::LevelWriter& level, BlockPos pos, FluidState fluid) const;

    /// Wake the fluids that were leaning on this position.
    void wake_neighbours(world::LevelWriter& level, BlockPos pos) const;

    /// Put `fluid` at `pos`, running the mixing table on the way in.
    void flow_into(world::LevelWriter& level, BlockPos pos, FluidState fluid) const;

    const registry::BlockRegistry* blocks_{nullptr};

    registry::BlockId water_{};
    registry::BlockId lava_{};
    /// The two `level` properties, one per fluid block. Resolved once: a lookup
    /// by name per spread is what makes a naive fluid engine the slowest thing
    /// in a server.
    registry::PropertyView water_level_{};
    registry::PropertyView lava_level_{};

    registry::BlockStateId air_{0};
    registry::BlockStateId stone_{0};
    registry::BlockStateId cobblestone_{0};
    registry::BlockStateId obsidian_{0};

    /// Per block: the `waterlogged` property, when it has one. Indexed by the
    /// pack's block index. Held as a stride rather than a PropertyView because
    /// that is all `with_property` needs and it fits in the cache.
    std::vector<u16> waterlogged_stride_;
};

}  // namespace ov::gameplay
