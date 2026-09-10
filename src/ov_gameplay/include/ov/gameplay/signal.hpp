// Who powers what: the redstone power model.
//
// Everything redstone does rests on four questions, and the reason circuits are
// hard to reimplement is that three of them have non-obvious answers.
//
//   1. **Which way does the arrow point?** Every query here takes a direction
//      that points *from the block being powered towards the source*. A lever
//      standing on a block is asked with `Up`; the block it powers is below it.
//      This is vanilla's convention and it reads backwards the first time. The
//      alternative — a direction pointing outwards — is the same code with
//      every call site inverted, and mixing the two is silent and total.
//
//   2. **Weak or strong.** A lever weakly powers all six of its neighbours and
//      strongly powers only the block it is stuck to. Weak power drives a lamp,
//      a piston, a door — anything that just asks "am I powered?". Strong power
//      is what a solid block *relays*: a block that is strongly powered by
//      anything becomes a power source of its own, at level 15, for wire and
//      repeaters alike. Without the distinction, half the circuits ever built
//      stop working, because "wire into a block, repeater out of the block" is
//      the standard way to cross a signal through a wall.
//
//   3. **Wire is invisible to wire, through blocks.** When a wire works out its
//      own power, every wire in the world is temporarily silent. That is why
//      wire → solid block → wire carries nothing, while wire → solid block →
//      repeater carries 15. It is not an extra rule bolted on; it is one flag
//      threaded through the same code, and it is modelled here as an explicit
//      parameter rather than the mutable static vanilla uses, because a mutable
//      global is the one thing this project does not have.
//
//   4. **What counts as solid.** "Redstone conductor" is not "opaque" and not
//      "has a collision box": glass conducts nothing, a slab conducts nothing,
//      and a piece of leaves conducts nothing even though it is a full cube.
//      The rule is a full collision cube *minus a measured exception list* —
//      measured, because the exceptions are Java code and no property of the
//      block predicts them.
#pragma once

#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/block_ticks.hpp"
#include "ov/world/level.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// The world as redstone sees it: a `LevelWriter`, plus one question.
///
/// Everything redstone needs — reading a state, writing one, asking to be woken
/// later, the game clock — is already on `world::LevelWriter`, and using it is
/// what lets a client replica run the same rules against its own storage.
///
/// The one addition is the **comparator's container reading**. A comparator
/// behind a chest outputs a level derived from how full it is, and a
/// `LevelWriter` cannot answer that: inventories live above this module, and a
/// comparator that had to know what a shulker box is would drag the whole item
/// system down into the gameplay layer. So it is asked for, as a number between
/// 0 and 15, with -1 meaning "there is no container there" — which a comparator
/// has to tell apart from "there is an empty one", since an empty container
/// gives 0 and a missing one lets the ordinary signal through.
class RedstoneWorld : public world::LevelWriter {
public:
    [[nodiscard]] virtual i32 container_signal(BlockPos pos) const = 0;

    /// The signal entities standing on a pressure plate at `pos` produce.
    ///
    /// Not pure, and the default is not a stand-in: a `RedstoneWorld` that
    /// carries no entities — a test map, a client replica that has not been
    /// given them — genuinely has nothing standing anywhere, and 0 is the true
    /// answer rather than a convenient one. A world that *does* carry entities
    /// overrides this, and a plate then behaves.
    ///
    /// The scale is the plate's own: 0 or 15 for the four plain plates, 0..15
    /// for the two weighted ones. Which of the two a plate wants is decided by
    /// the plate, not by the caller.
    [[nodiscard]] virtual i32 entity_pressure(BlockPos pos) const {
        (void)pos;
        return 0;
    }
};

/// What a block does when asked for a signal.
///
/// Resolved once per block at construction, so the hot path is an array index
/// rather than a string comparison. Blocks not listed emit nothing.
enum class SignalKind : u8 {
    None,
    /// Emits 15 in every direction, always.
    RedstoneBlock,
    /// `lit`: 15 everywhere except through the face it stands on, and strongly
    /// upwards.
    Torch,
    /// The wall variant, which also skips the wall it is stuck to.
    WallTorch,
    /// `powered` and an attachment face: lever and all buttons.
    Attached,
    /// `powered`, strong downwards: the four plain pressure plates.
    PressurePlate,
    /// `power` 0-15, strong downwards: the two weighted plates.
    WeightedPlate,
    /// `powered`, strong along `facing`: the tripwire hook.
    TripwireHook,
    /// Repeater and comparator: output only along `facing`, strongly.
    Diode,
    /// `powered`, strong out of the back.
    Observer,
    /// `power` 0-15, weak only.
    Analogue,
    /// `powered`, strong downwards: the detector rail.
    DetectorRail,
    /// Power gated by the connection shape; silent to other wire.
    Wire,
    /// `powered`, strong along `facing`: the lightning rod.
    LightningRod,
};

/// The power model.
///
/// Holds no mutable state: every answer is a pure function of the world it is
/// handed. That is what lets the client run it speculatively and throw the
/// result away.
class Signals {
public:
    Signals(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    [[nodiscard]] SignalKind kind_of(registry::BlockId block) const noexcept;

    /// Does this state conduct redstone — that is, does it relay strong power
    /// as a source of its own, and does wire climb over it?
    [[nodiscard]] bool is_conductor(registry::BlockStateId state) const noexcept;

    /// Is this state a full collision cube? The starting point for the above,
    /// exposed because pistons ask a related but different question.
    [[nodiscard]] bool is_full_cube(registry::BlockStateId state) const noexcept;

    // ── The four primitives ─────────────────────────────────────────────────
    //
    // `from` always points from the powered block towards the source, so the
    // block that a source at `pos` powers along `from` is `pos.offset(opposite
    // (from))`. See the note at the top of this file.

    /// Weak power the block at `pos` puts out along `from`.
    ///
    /// `wires_signal` false silences every wire, which is the state the world
    /// is in while a wire computes its own strength.
    [[nodiscard]] i32 weak_signal(const RedstoneWorld& world, BlockPos pos, Direction from,
                                  bool wires_signal = true) const;

    /// Strong power the block at `pos` puts out along `from`.
    [[nodiscard]] i32 strong_signal(const RedstoneWorld& world, BlockPos pos, Direction from,
                                    bool wires_signal = true) const;

    /// What the block at `pos` offers its neighbour along `from`, including the
    /// strong power a conductor relays. Vanilla's `Level.getSignal`.
    [[nodiscard]] i32 signal_at(const RedstoneWorld& world, BlockPos pos, Direction from,
                                bool wires_signal = true) const;

    /// The strongest signal reaching `pos` from any of its six neighbours.
    [[nodiscard]] i32 best_neighbour_signal(const RedstoneWorld& world, BlockPos pos,
                                            bool wires_signal = true) const;

    /// The strongest **strong** power reaching `pos`. What makes a solid block
    /// "powered".
    [[nodiscard]] i32 direct_signal_to(const RedstoneWorld& world, BlockPos pos,
                                       bool wires_signal = true) const;

    /// Is anything at all powering `pos`? The question a lamp, a door and a
    /// piston ask.
    [[nodiscard]] bool has_neighbour_signal(const RedstoneWorld& world, BlockPos pos) const;

    /// The output a repeater or comparator at `pos` currently emits, read from
    /// its own state rather than recomputed.
    [[nodiscard]] i32 diode_output(registry::BlockStateId state) const noexcept;

    // ── Named lookups, resolved once ────────────────────────────────────────

    [[nodiscard]] registry::BlockId wire_block() const noexcept { return wire_; }

    [[nodiscard]] const registry::BlockRegistry& blocks() const noexcept { return *blocks_; }

    /// The `power` property value of a state, or -1 for a block without one.
    [[nodiscard]] i32 power_of(registry::BlockStateId state) const noexcept;

    /// Set the `power` property, which the wire and the plates all need.
    [[nodiscard]] registry::BlockStateId with_power(registry::BlockStateId state,
                                                    i32                    power) const noexcept;

    /// Read and write a boolean property by index, for `powered`, `lit`,
    /// `locked`, `extended` and `triggered`.
    [[nodiscard]] bool flag_of(registry::BlockStateId state, std::string_view name) const noexcept;
    [[nodiscard]] registry::BlockStateId with_flag(registry::BlockStateId state,
                                                   std::string_view name, bool value) const noexcept;

    /// The horizontal or full `facing` of a state, or nothing when it has none.
    [[nodiscard]] std::optional<Direction> facing_of(registry::BlockStateId state) const noexcept;

    /// The direction a lever or button points, derived from `face` and
    /// `facing`: up for a floor switch, down for a ceiling one, and the facing
    /// itself for a wall.
    [[nodiscard]] Direction attached_direction(registry::BlockStateId state) const noexcept;

    /// Blocks that vanilla treats as power sources and this model does not.
    ///
    /// Named rather than left to behave as zero. A test walks this list, so a
    /// gap is a failing assertion with a block name in it and not a circuit
    /// that quietly does nothing.
    [[nodiscard]] std::span<const std::string_view> unhandled_sources() const noexcept {
        return unhandled_;
    }

private:
    [[nodiscard]] i32 wire_signal(const RedstoneWorld& world, BlockPos pos,
                                  registry::BlockStateId state, Direction from) const;

    const registry::BlockRegistry* blocks_{nullptr};

    std::vector<SignalKind> kinds_;
    /// Per state: full collision cube, and conductor.
    std::vector<bool> full_cube_;
    std::vector<bool> conductor_;

    registry::BlockId wire_{0};

    std::vector<std::string_view> unhandled_;
};

}  // namespace ov::gameplay
