// Moving one item at a time: the hopper, and the container vocabulary it needs.
//
// The redstone model of this project drives every consumer's `powered` flag
// correctly and, until this file existed, **nothing acted on any of them**. The
// hopper is the first of those effects, and it is the one most real circuits
// lean on: a sorter, a furnace array, an item elevator and a clock are all
// hoppers with a signal attached.
//
// ── Why a container interface and not the server's chests ───────────────────
//
// `ov_gameplay` may not know what a chest is. Inventories live in `ov_server`
// as NBT on a block entity, and a rule that reached for them would drag the
// whole item system down a layer and make client-side prediction impossible.
// So the rules here take an abstract `ItemContainer`, and the caller — the
// server today, a client replica tomorrow — implements it over whatever it
// actually stores. Everything below is then testable against a hand-written
// container, which is what the unit tests do.
//
// ── The four things a hopper does, in the order it does them ────────────────
//
//   1. **push** one item into the container it faces;
//   2. **pull** one item from the container above it;
//   3. **collect** whole item entities resting on it;
//   4. and if any of the three moved anything, **wait**.
//
// The wait is the whole reason a hopper chain has a rate at all, and its value
// is measured rather than assumed: see `kTransferCooldown` and
// docs/provenance/redstone.md.
//
// ── Sided access ────────────────────────────────────────────────────────────
//
// A container does not offer every slot to every face. A furnace gives up its
// output from below and takes fuel from the side; a brewing stand is worse. The
// interface asks the container, rather than tabulating which block is which
// here — the layer that owns the inventory is the layer that knows.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/registries.hpp"

#include <span>
#include <vector>

namespace ov::gameplay {

/// One stack, as the item-moving rules see it.
///
/// Deliberately not the protocol's `ItemStack`: `ov_gameplay` sits below
/// `ov_protocol` and has no business knowing what a slot looks like on the
/// wire. `RecipeStack` made the same choice for the same reason.
///
/// `tag` is the one concession to NBT. Two stacks merge only when their item
/// **and** their tag match — a named sword does not stack with a plain one —
/// and the rules here never need to know what is inside it, only whether two
/// are the same. The caller hashes its own NBT into it; 0 means "no tag".
struct SlotStack {
    registry::ProtocolId item{0};
    i32                  count{0};
    u64                  tag{0};

    [[nodiscard]] bool empty() const noexcept { return count <= 0; }

    /// Do these two stacks merge? Empty stacks never do — an empty slot is
    /// filled, not merged into, and the two paths differ.
    [[nodiscard]] bool merges_with(const SlotStack& other) const noexcept {
        return !empty() && !other.empty() && item == other.item && tag == other.tag;
    }

    friend constexpr bool operator==(const SlotStack&, const SlotStack&) noexcept = default;
};

/// A container of slots.
///
/// The four questions a hopper asks and nothing more. An implementation over a
/// chest answers all four trivially; one over a furnace uses `face` on the last
/// two and that is the whole of sided access.
class ItemContainer {
public:
    ItemContainer()                                = default;
    ItemContainer(const ItemContainer&)            = delete;
    ItemContainer& operator=(const ItemContainer&) = delete;
    ItemContainer(ItemContainer&&)                 = delete;
    ItemContainer& operator=(ItemContainer&&)      = delete;
    virtual ~ItemContainer()                       = default;

    [[nodiscard]] virtual i32 slot_count() const = 0;

    [[nodiscard]] virtual SlotStack slot(i32 index) const = 0;

    virtual void set_slot(i32 index, const SlotStack& stack) = 0;

    /// The largest stack this item makes: 64 for most, 16 for an egg, 1 for a
    /// bucket. Asked of the container because the registry that knows lives
    /// above the rule that asks.
    [[nodiscard]] virtual i32 max_stack(registry::ProtocolId item) const = 0;

    /// May a hopper take from this slot, reaching in through `face`?
    ///
    /// `face` points from the container towards the hopper — vanilla's
    /// convention, the same one `Signals` uses, and the same one it reads
    /// backwards the first time.
    [[nodiscard]] virtual bool can_take_from(i32 index, Direction face) const {
        (void)index;
        (void)face;
        return true;
    }

    /// May a hopper put `stack` into this slot through `face`?
    [[nodiscard]] virtual bool can_place_into(i32 index, const SlotStack& stack,
                                              Direction face) const {
        (void)index;
        (void)stack;
        (void)face;
        return true;
    }
};

/// A plain container: every slot, both ways, no sided access.
///
/// A chest, a dropper, a dispenser, a barrel, a shulker box and a hopper are
/// all this. Provided here because otherwise every caller and every test
/// writes it again.
class SimpleContainer final : public ItemContainer {
public:
    SimpleContainer(i32 slots, i32 stack_limit)
        : slots_(static_cast<usize>(slots < 0 ? 0 : slots)), limit_{stack_limit} {}

    [[nodiscard]] i32 slot_count() const override { return static_cast<i32>(slots_.size()); }

    [[nodiscard]] SlotStack slot(i32 index) const override {
        return in_range(index) ? slots_[static_cast<usize>(index)] : SlotStack{};
    }

    void set_slot(i32 index, const SlotStack& stack) override {
        if (in_range(index)) {
            slots_[static_cast<usize>(index)] = stack;
        }
    }

    [[nodiscard]] i32 max_stack(registry::ProtocolId item) const override {
        (void)item;
        return limit_;
    }

    /// The total count across every slot. Not part of the interface: a rule
    /// never needs it, and a test always does.
    [[nodiscard]] i32 total() const noexcept {
        i32 sum = 0;
        for (const SlotStack& stack : slots_) {
            sum += stack.count;
        }
        return sum;
    }

private:
    [[nodiscard]] bool in_range(i32 index) const noexcept {
        return index >= 0 && static_cast<usize>(index) < slots_.size();
    }

    std::vector<SlotStack> slots_;
    i32                    limit_{64};
};

/// The hopper rules.
///
/// Stateless: everything a hopper remembers between ticks is its cooldown, and
/// that belongs to the hopper and not to the rules. The caller keeps it — as a
/// field on its block entity, exactly where vanilla keeps it.
class HopperRules {
public:
    /// How many slots a hopper has.
    static constexpr i32 kSlots = 5;

    /// Ticks a hopper waits after moving anything.
    ///
    /// **Measured**: a chest → hopper → chest rig moved 64 cobblestone in a
    /// span the server itself timed, and the slope is in
    /// docs/provenance/redstone.md. It is the number the whole of hopper
    /// timing rests on: a chain of hoppers moves one item per this many ticks,
    /// and every item filter ever built is that rate against another.
    static constexpr i32 kTransferCooldown = 8;

    /// Move at most one item from `from` into `into`.
    ///
    /// `face` points from `into` back towards `from`, which is what a sided
    /// container is asked about. Returns true if an item moved.
    [[nodiscard]] static bool push_one(ItemContainer& from, ItemContainer& into, Direction face);

    /// Move at most one item from `above` into `hopper`.
    ///
    /// Separate from `push_one` only in which face it presents: a hopper
    /// reaching up takes from the container's **bottom**, so the face it shows
    /// is `Down`.
    [[nodiscard]] static bool pull_one(ItemContainer& hopper, ItemContainer& above);

    /// Put as much of `stack` into `into` as fits, through `face`.
    ///
    /// What an item entity falling onto a hopper does, and what a dropper's
    /// eject does when it faces a container. `stack` is left holding whatever
    /// did not fit — nothing is destroyed, and a caller that ignores the
    /// remainder loses items rather than silently succeeding.
    static i32 insert(ItemContainer& into, SlotStack& stack, Direction face);

    /// Everything one hopper does in one tick, in vanilla's order.
    ///
    /// `enabled` is the block state flag — false while the hopper is powered.
    /// `facing` is where it points. `target` and `source` are the containers on
    /// either side, either of which may be null when there is no container
    /// there.
    ///
    /// Returns true if anything moved, which is exactly when the caller must
    /// reset the cooldown to `kTransferCooldown`.
    struct TickResult {
        bool pushed{false};
        bool pulled{false};

        [[nodiscard]] bool moved() const noexcept { return pushed || pulled; }
    };

    [[nodiscard]] static TickResult tick(ItemContainer& hopper, ItemContainer* target,
                                         Direction facing, ItemContainer* source);

    /// The first slot of `into` that would accept `stack`, or -1.
    ///
    /// Public because it is the question a test asks, and because a dispenser
    /// facing a container asks it too.
    [[nodiscard]] static i32 slot_for(const ItemContainer& into, const SlotStack& stack,
                                      Direction face);
};

}  // namespace ov::gameplay
