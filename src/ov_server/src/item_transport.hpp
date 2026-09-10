// The machines that move items on their own: the hopper, the dropper, the
// dispenser.
//
// Three rule sets sat finished and unreachable in `ov_gameplay` before this
// file: `HopperRules` with nine passing tests and a measured eight-tick
// cooldown, `Dispenser` with an eighteen-item behaviour table read off a real
// server, and the `enabled` / `triggered` flags the redstone engine has been
// driving correctly all along with nothing on the other end. What was missing
// is the same thing that was missing for fluids and redstone before
// `world_ticks.cpp`: the pass that walks the world once a tick and hands the
// rules something real.
//
// ── Why an index rather than a scan ─────────────────────────────────────────
//
// A hopper has to be ticked every tick; a loaded chunk holds 98 304 blocks and
// a view distance of ten holds 441 of them. Scanning for hoppers per tick is
// twenty million reads for a world that usually holds none. So the machines in
// loaded chunks are indexed from their block entities — which a chunk already
// keeps as a short list — and the index is rebuilt once a second, exactly as
// the furnace pass does and for the same reason.
//
// ── The rising edge ─────────────────────────────────────────────────────────
//
// A dispenser does not fire while `triggered` is true; it fires **when it
// becomes** true. The flag is set by the redstone consumer pass, so the edge is
// detected where the write happens — `note_block_change` below — and the firing
// happens four ticks later, which is vanilla's own delay and is what makes a
// dispenser under a repeater fire once rather than every tick the lever is up.
#pragma once

#include "block_container.hpp"

#include "ov/gameplay/dispenser.hpp"
#include "ov/gameplay/hopper.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ov::server {

/// Does the hopper at `hopper` reach an item entity resting at this position?
///
/// Exported because the server owns the item entity list and this file owns the
/// geometry, and putting the geometry where the list is would make it the
/// fourth place in this project that guesses at a bounding box.
[[nodiscard]] bool hopper_suck_contains(BlockPos hopper, f64 x, f64 y, f64 z) noexcept;

/// Everything the pass reaches outside itself for.
///
/// Callbacks rather than a reference to the server, for the reason
/// `WorkbenchHost` gives: this file must not know what a connection, a player
/// or a chunk map is.
struct TransportHost {
    /// The chunk at these chunk coordinates, or nullptr when it is not
    /// resident. Never generating: a hopper must not pull three hundred
    /// milliseconds of worldgen onto the tick thread.
    std::function<world::Chunk*(i32, i32)> chunk;

    /// This chunk changed and has to be saved.
    std::function<void(i32, i32)> mark_dirty;

    /// Throw a stack into the world in front of a machine.
    std::function<void(BlockPos, Direction, const net::ItemStack&)> eject;

    /// Offer every item entity a hopper could swallow at this position.
    ///
    /// The callback is handed each candidate stack and returns true when it
    /// took something from it — the caller then removes the entity if the stack
    /// is empty. One item per hopper per cycle, which is what the visitor
    /// returning after the first true enforces.
    std::function<bool(BlockPos, const std::function<bool(net::ItemStack&)>&)> collect;

    /// A container at this position changed while somebody had it open.
    std::function<void(BlockPos)> container_changed;
};

/// What one pass did. Instrumentation, and the numbers the provenance note is
/// written from.
struct TransportStats {
    usize hoppers{0};
    /// Hoppers that moved something this tick — the numerator of the rate.
    usize hopper_moves{0};
    usize collected{0};
    usize fired{0};
    /// Dispense behaviours named and not carried out. Counted so that "never
    /// happens" is a measurement rather than a hope.
    usize unsupported{0};
};

/// The pass, and the small amount of state it has to keep between ticks.
class ItemTransport {
public:
    ItemTransport(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// A block was written. Watch for a dispenser or dropper whose `triggered`
    /// flag has just gone up.
    ///
    /// Called with the state **before** and the state after, because the edge
    /// is the difference and reading the world again here would already see the
    /// new one.
    void note_block_change(BlockPos pos, registry::BlockStateId before,
                           registry::BlockStateId after);

    /// One tick. `loaded` names the chunks to index, and is only walked when
    /// the index is due to be rebuilt.
    TransportStats tick(const TransportHost& host, std::span<const ChunkPos> loaded, i64 now);

    /// Behaviours the dispenser table names and this pass does not carry out.
    ///
    /// Named rather than silently ejected, which is this project's rule: a
    /// dispenser that drops a water bucket on the floor instead of placing
    /// water looks exactly like a dispenser that works.
    [[nodiscard]] std::span<const std::string> unsupported() const noexcept {
        return unsupported_;
    }

    /// How often the machine index is rebuilt, in ticks.
    static constexpr i64 kIndexPeriod = 20;

    /// Ticks between a dispenser being triggered and firing. Vanilla schedules
    /// its own tick four ticks out; without the delay a dispenser fires in the
    /// same tick the lever moves, which is one tick early and visible against a
    /// repeater.
    static constexpr i64 kFireDelay = 4;

private:
    struct Machine {
        BlockPos pos;
    };

    struct PendingFire {
        BlockPos pos;
        i64      at{0};
    };

    /// The container at a position, loaded, with its block entity remembered so
    /// that the result can be written back.
    struct LoadedContainer {
        world::Chunk*        chunk{nullptr};
        world::BlockEntity*  entity{nullptr};
        const ContainerSpec* spec{nullptr};
        BlockInventory       inventory;
    };

    [[nodiscard]] bool open_container(const TransportHost& host, BlockPos pos,
                                      LoadedContainer& out) const;

    void store_container(const TransportHost& host, LoadedContainer& container, BlockPos pos) const;

    void rebuild_index(const TransportHost& host, std::span<const ChunkPos> loaded);

    void tick_hopper(const TransportHost& host, BlockPos pos, TransportStats& stats);

    void fire_machine(const TransportHost& host, BlockPos pos, TransportStats& stats);

    void name_unsupported(std::string what);

    const registry::BlockRegistry*      blocks_{nullptr};
    const registry::Registries*         registries_{nullptr};
    std::optional<registry::RegistryId> item_registry_{};
    gameplay::Dispenser                 dispenser_;

    /// The hopper and machine blocks, by block id, so the hot check is an
    /// integer compare rather than a string one.
    std::optional<registry::BlockId> hopper_block_{};
    std::optional<registry::BlockId> dispenser_block_{};
    std::optional<registry::BlockId> dropper_block_{};

    std::vector<Machine>     hoppers_;
    std::vector<PendingFire> pending_;
    i64                      indexed_at_{-1};

    /// Reused between ticks so that the pass allocates nothing in steady state.
    TagPool tags_;

    std::vector<std::string> unsupported_;
};

}  // namespace ov::server
