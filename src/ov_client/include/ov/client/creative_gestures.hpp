// What a click, a key or a throw does in the creative inventory — decided
// here, without a device and without a socket, so every rule is a unit test.
//
// In creative the client is authoritative for its own inventory: vanilla's
// creative screen moves the stacks itself and then tells the server, slot by
// slot, with Set Creative Mode Slot (0x2B). A thrown stack is the same packet
// with slot −1. This class is that client-side half. It keeps the mirror of
// window 0 and the cursor, applies a gesture, and returns the packets the
// gesture implies; the caller sends them.
//
// Every rule below was **measured on the running 1.20.1 client**
// (scripts/measure_creative_screen.py, docs/provenance/inventaire-creatif.md
// § 8), except the two marked otherwise:
//
//   catalogue cell, empty hand     left or right: one item; middle or shift:
//                                  a full stack — on the cursor, not in the
//                                  hotbar; number key N: a full stack in
//                                  hotbar slot N; Q: throws one
//   catalogue cell, stack in hand  same item: left adds one, right takes one
//                                  back; another item: the stack in hand is
//                                  deleted; middle and number keys do nothing;
//                                  Q still throws a copy of the cell
//   player slot                    the container rules (pick up, place, merge,
//                                  swap, right-click halves and places one);
//                                  shift on a category page's hotbar row
//                                  deletes the stack; Q throws one
//   outside the panel              left throws the stack in hand, right one
//   destroy slot                   deletes the stack in hand; shift empties
//                                  the whole inventory
//
// Not measured, named: the shift-click on the *survival* page (vanilla's
// inventory quick-move is used: hotbar ↔ inventory, armour to its slot), and
// the size of a Ctrl+Q throw from a cell (a full stack, as from a slot).
#pragma once

#include "ov/base/types.hpp"

#include <array>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace ov::client {

/// A stack as the creative screen moves it: the protocol item id, the count,
/// and the Slot's NBT bytes (TAG_Compound, empty name, payload — or nothing).
struct SlotStack {
    i32             item{0};
    i32             count{0};
    std::vector<u8> nbt;

    [[nodiscard]] bool empty() const noexcept { return item <= 0 || count <= 0; }

    /// Same item and same NBT: the two may share a slot.
    [[nodiscard]] bool same_kind(const SlotStack& other) const noexcept {
        return item == other.item && nbt == other.nbt;
    }

    [[nodiscard]] SlotStack with_count(i32 n) const {
        SlotStack out = *this;
        out.count     = n;
        return out;
    }
};

/// What one gesture asks of the server. Both lists travel as Set Creative
/// Mode Slot: a slot change names the window-0 slot, a throw is slot −1.
struct CreativeEffects {
    std::vector<std::pair<i16, SlotStack>> slots;
    std::vector<SlotStack>                 drops;

    void clear() {
        slots.clear();
        drops.clear();
    }
};

/// Which page the pointer is on. On a page of items only the hotbar row is
/// the player's; on the survival page everything is, and shift means
/// something else.
enum class CreativePage : u8 { Items, Survival };

class CreativeInventory {
public:
    /// Window 0: result, 4 crafting, 4 armour (5 head … 8 feet), 27, 9 hotbar
    /// (36..44), off hand (45).
    static constexpr usize kSlots       = 46;
    static constexpr i16   kHotbarFirst = 36;
    static constexpr i16   kOffHand     = 45;

    /// The item's stack limit: 64, 16, or 1.
    using MaxStack = std::function<i32(i32 item)>;
    /// The armour slot (5..8) an item may be worn in, −1 for none, or −2 for
    /// "unknown" (no measured data), which the armour slots then accept. With
    /// data they refuse anything else, in creative as in survival.
    using ArmourSlot = std::function<i32(const SlotStack& stack)>;

    CreativeInventory();

    void set_rules(MaxStack max_stack, ArmourSlot armour_slot);

    [[nodiscard]] std::span<SlotStack> slots() noexcept { return slots_; }
    [[nodiscard]] std::span<const SlotStack> slots() const noexcept { return slots_; }

    [[nodiscard]] SlotStack&       carried() noexcept { return carried_; }
    [[nodiscard]] const SlotStack& carried() const noexcept { return carried_; }

    // ── The catalogue ───────────────────────────────────────────────────────

    /// `button` 0 left, 1 right, 2 middle.
    void click_cell(const SlotStack& cell, i32 button, bool shift, CreativeEffects& effects);
    void hotbar_key_on_cell(const SlotStack& cell, i32 hotbar, CreativeEffects& effects);
    void throw_cell(const SlotStack& cell, bool whole_stack, CreativeEffects& effects);

    // ── The player's slots ──────────────────────────────────────────────────

    void click_slot(i16 slot, i32 button, bool shift, CreativePage page, CreativeEffects& effects);
    void hotbar_key_on_slot(i16 slot, i32 hotbar, CreativeEffects& effects);
    void throw_slot(i16 slot, bool whole_stack, CreativeEffects& effects);

    // ── Everything else ─────────────────────────────────────────────────────

    void click_outside(i32 button, CreativeEffects& effects);
    void click_destroy(bool shift, CreativeEffects& effects);

    /// Whether `stack` may go into `slot`: the armour slots take only what
    /// fits them.
    [[nodiscard]] bool may_place(i16 slot, const SlotStack& stack) const;

    [[nodiscard]] i32 max_stack(const SlotStack& stack) const;

private:
    void set(i16 slot, SlotStack stack, CreativeEffects& effects);
    [[nodiscard]] i32 slot_limit(i16 slot, const SlotStack& stack) const;
    /// Move `slot`'s stack into the range [first, last), merging first and
    /// then filling empties, as the inventory's quick-move does.
    void move_into(i16 slot, i16 first, i16 last, CreativeEffects& effects);

    std::array<SlotStack, kSlots> slots_{};
    SlotStack                     carried_;
    MaxStack                      max_stack_;
    ArmourSlot                    armour_slot_;
};

}  // namespace ov::client
