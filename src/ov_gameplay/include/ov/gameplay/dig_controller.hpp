// The client's half of breaking a block: what it sends, what it shows, when.
//
// The server decides whether a block comes off (breaking.hpp); the *client*
// decides when to ask, and it animates the cracks from its own count while it
// waits. A client that asks at a different tick than vanilla's is one the
// server either refuses (too early: below the 0.7 threshold, the block stays
// and the server finishes it on its own clock) or one that visibly lags. So
// this is the vanilla client's schedule, one client tick (20 Hz) at a time:
//
//   * a **press** on a block sends Start (Player Action 0). In survival a block
//     whose per-tick progress is 1 or more comes off at once; otherwise the
//     count begins at zero;
//   * **holding** the button advances the count by one tick's progress, the
//     press's own tick included, and plays the hit sound every fourth tick
//     of the count;
//   * the count reaching 1 sends Finish (Player Action 2), breaks the block on
//     this side, and starts a **delay of 5 ticks** during which nothing is
//     counted; the next block is started on the tick after it — the 6-tick gap
//     between blocks the wiki's Breaking page states;
//   * turning to another block while holding aborts the first (Player Action 1)
//     and starts the second; letting go or looking at nothing aborts;
//   * in creative a block comes off on the press and then one every 6 ticks
//     while held: the same delay, and the hold of the press's own tick finds
//     air where the press broke the block, so it spends none of it.
//
// The crack stage shown is floor(count * 10) - 1: nothing below a tenth, then
// 0 to 9. What the *server* tells the others is a different number —
// floor(its own count * 10), 0 from the first tick and past 9 when the client
// is slow to claim (docs/provenance/cassage-bloc.md, measured on 1.20.1).
//
// Pure: no registry lookups, no network, no clock. The caller resolves the
// per-tick progress through BreakRules::destroy_progress — the same function
// the server counts with, so the two cannot disagree by construction.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"

#include <array>
#include <optional>

namespace ov::gameplay {

/// Player Action's first three statuses, with the protocol's numbers.
enum class DigStatus : u8 { Start = 0, Abort = 1, Finish = 2 };

/// One Player Action to send.
struct DigAction {
    DigStatus status{DigStatus::Start};
    BlockPos  pos{};
    /// The face, numbered as the protocol numbers it (0 down .. 5 east).
    u8 face{1};
};

/// What the crosshair is on this tick.
struct DigTarget {
    BlockPos               pos{};
    u8                     face{1};
    registry::BlockStateId state{};
    /// Air, water and lava are not dug. Filled by the caller, which knows.
    bool diggable{true};
};

/// One client tick of input.
struct DigInput {
    /// The attack button went down since the last tick.
    bool pressed{false};
    /// The attack button is down now.
    bool held{false};
    std::optional<DigTarget> target;
    bool creative{false};
    /// False for what cannot break blocks in creative (a sword, a trident,
    /// the debug stick). Ignored in survival.
    bool can_attack_in_creative{true};
    /// BreakRules::destroy_progress for the target with what is held now.
    f32 progress_per_tick{0.0F};
    /// Identity of the held stack. A change mid-dig restarts the block, as
    /// vanilla's "same target" test includes the item.
    i32 held_item{-1};
};

/// What one tick produced.
struct DigOutcome {
    /// Player Actions, in the order they must be sent.
    std::array<DigAction, 3> actions{};
    u8                       action_count{0};

    /// The arm swings (and Swing Arm is sent) this tick.
    bool swing{false};
    /// One crack particle on the target's face this tick.
    bool crack_particle{false};
    /// The hit sound of the block being counted plays this tick.
    bool hit_sound{false};

    /// The block this client broke this tick, and what it was: the break
    /// sound, the destroy particles and the local prediction hang off it.
    std::optional<BlockPos> broken;
    registry::BlockStateId  broken_state{};

    void push(DigStatus status, BlockPos pos, u8 face) noexcept {
        if (action_count < actions.size()) {
            actions[action_count++] = DigAction{status, pos, face};
        }
    }
};

class DigController {
public:
    /// Ticks between breaking a block and counting the next. Vanilla's.
    static constexpr i32 kDestroyDelay = 5;

    /// One client tick.
    DigOutcome tick(const DigInput& input);

    /// The crack stage this client draws on its own target, -1 for none.
    [[nodiscard]] i32 stage() const noexcept;
    /// The block being counted, if any.
    [[nodiscard]] std::optional<BlockPos> digging() const noexcept;
    [[nodiscard]] f32 progress() const noexcept { return progress_; }
    [[nodiscard]] i32 delay() const noexcept { return delay_; }

    /// Forget everything without sending anything: a disconnect, a respawn.
    void reset() noexcept { *this = DigController{}; }

private:
    void start(const DigInput& input, const DigTarget& target, DigOutcome& out);
    bool advance(const DigInput& input, const DigTarget& target, DigOutcome& out);
    void stop(DigOutcome& out);
    void destroy(const DigTarget& target, DigOutcome& out);

    bool     destroying_{false};
    BlockPos pos_{};
    u8       face_{1};
    i32      item_{-1};
    f32      progress_{0.0F};
    i32      ticks_{0};
    i32      delay_{0};
};

}  // namespace ov::gameplay
