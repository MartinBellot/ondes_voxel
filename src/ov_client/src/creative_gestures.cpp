#include "ov/client/creative_gestures.hpp"

#include <algorithm>

namespace ov::client {

CreativeInventory::CreativeInventory()
    : max_stack_([](i32) { return 64; }), armour_slot_([](const SlotStack&) { return -1; }) {}

void CreativeInventory::set_rules(MaxStack max_stack, ArmourSlot armour_slot) {
    if (max_stack) {
        max_stack_ = std::move(max_stack);
    }
    if (armour_slot) {
        armour_slot_ = std::move(armour_slot);
    }
}

i32 CreativeInventory::max_stack(const SlotStack& stack) const {
    return std::max(1, max_stack_(stack.item));
}

bool CreativeInventory::may_place(i16 slot, const SlotStack& stack) const {
    if (slot >= 5 && slot <= 8) {
        // −2 is "unknown": without the measured data the slot refuses
        // nothing, rather than everything.
        const i32 wanted = armour_slot_(stack);
        return !stack.empty() && (wanted == -2 || wanted == slot);
    }
    // The crafting result and grid are never shown by the creative screen.
    return slot >= 9 && slot <= kOffHand;
}

i32 CreativeInventory::slot_limit(i16 slot, const SlotStack& stack) const {
    // An armour slot holds one piece, whatever the item's own limit.
    return slot >= 5 && slot <= 8 ? 1 : max_stack(stack);
}

void CreativeInventory::set(i16 slot, SlotStack stack, CreativeEffects& effects) {
    if (stack.count <= 0) {
        stack = SlotStack{};
    }
    SlotStack& target = slots_[static_cast<usize>(slot)];
    if (target.empty() && stack.empty()) {
        return;
    }
    target = stack;
    effects.slots.emplace_back(slot, std::move(stack));
}

// ── The catalogue ───────────────────────────────────────────────────────────

void CreativeInventory::click_cell(const SlotStack& cell, i32 button, bool shift,
                                   CreativeEffects& effects) {
    (void)effects;  // the cursor is not a slot: nothing reaches the server
    if (cell.empty()) {
        return;
    }
    const i32 full = max_stack(cell);
    if (button == 2) {
        if (carried_.empty()) {
            carried_ = cell.with_count(full);
        }
        return;
    }
    if (shift) {
        carried_ = cell.with_count(full);
        return;
    }
    if (carried_.empty()) {
        carried_ = cell.with_count(1);
        return;
    }
    if (!carried_.same_kind(cell)) {
        carried_ = SlotStack{};
        return;
    }
    if (button == 0) {
        carried_.count = std::min(full, carried_.count + 1);
    } else {
        carried_.count -= 1;
        if (carried_.count <= 0) {
            carried_ = SlotStack{};
        }
    }
}

void CreativeInventory::hotbar_key_on_cell(const SlotStack& cell, i32 hotbar,
                                           CreativeEffects& effects) {
    if (cell.empty() || !carried_.empty() || hotbar < 0 || hotbar > 8) {
        return;
    }
    set(static_cast<i16>(kHotbarFirst + hotbar), cell.with_count(max_stack(cell)), effects);
}

void CreativeInventory::throw_cell(const SlotStack& cell, bool whole_stack,
                                   CreativeEffects& effects) {
    if (cell.empty()) {
        return;
    }
    effects.drops.push_back(cell.with_count(whole_stack ? max_stack(cell) : 1));
}

// ── The player's slots ──────────────────────────────────────────────────────

void CreativeInventory::move_into(i16 slot, i16 first, i16 last, CreativeEffects& effects) {
    SlotStack moving = slots_[static_cast<usize>(slot)];
    if (moving.empty()) {
        return;
    }
    // Merge into stacks of the same kind first, then fill the first empties.
    for (i16 target = first; target < last && moving.count > 0; ++target) {
        SlotStack& there = slots_[static_cast<usize>(target)];
        if (target == slot || there.empty() || !there.same_kind(moving)) {
            continue;
        }
        const i32 room = slot_limit(target, there) - there.count;
        const i32 put  = std::min(room, moving.count);
        if (put > 0) {
            set(target, there.with_count(there.count + put), effects);
            moving.count -= put;
        }
    }
    for (i16 target = first; target < last && moving.count > 0; ++target) {
        if (target == slot || !slots_[static_cast<usize>(target)].empty() ||
            !may_place(target, moving)) {
            continue;
        }
        const i32 put = std::min(slot_limit(target, moving), moving.count);
        set(target, moving.with_count(put), effects);
        moving.count -= put;
    }
    set(slot, moving, effects);
}

void CreativeInventory::click_slot(i16 slot, i32 button, bool shift, CreativePage page,
                                   CreativeEffects& effects) {
    if (slot < 0 || static_cast<usize>(slot) >= kSlots) {
        return;
    }
    SlotStack& there = slots_[static_cast<usize>(slot)];

    if (shift && button != 2) {
        if (page == CreativePage::Items) {
            // Measured: the hotbar row of a category page has nowhere to
            // quick-move to, and the stack is simply gone.
            set(slot, SlotStack{}, effects);
            return;
        }
        // The survival page: vanilla's inventory quick-move (not measured —
        // GLFW cannot fake a held shift, see the header). Armour goes to its
        // own slot first, then hotbar ↔ inventory.
        const SlotStack moving = there;
        if (moving.empty()) {
            return;
        }
        const i32 armour = armour_slot_(moving);
        if (slot >= 9 && armour >= 5 && armour <= 8 &&
            slots_[static_cast<usize>(armour)].empty()) {
            set(static_cast<i16>(armour), moving.with_count(1), effects);
            set(slot, moving.with_count(moving.count - 1), effects);
            return;
        }
        if (slot >= kHotbarFirst && slot < kOffHand) {
            move_into(slot, 9, kHotbarFirst, effects);
        } else if (slot >= 9 && slot < kHotbarFirst) {
            move_into(slot, kHotbarFirst, kOffHand, effects);
        } else {
            move_into(slot, 9, kOffHand, effects);
        }
        return;
    }

    if (button == 2) {
        // Middle click clones, in creative, when the hand is empty.
        if (carried_.empty() && !there.empty()) {
            carried_ = there.with_count(max_stack(there));
        }
        return;
    }

    if (carried_.empty()) {
        if (there.empty()) {
            return;
        }
        if (button == 0) {
            carried_ = there;
            set(slot, SlotStack{}, effects);
        } else {
            // Right click takes the larger half.
            const i32 take = (there.count + 1) / 2;
            carried_       = there.with_count(take);
            set(slot, there.with_count(there.count - take), effects);
        }
        return;
    }

    if (!may_place(slot, carried_)) {
        return;
    }
    const i32 limit = slot_limit(slot, carried_);
    if (there.empty() || there.same_kind(carried_)) {
        const i32 room = limit - (there.empty() ? 0 : there.count);
        const i32 put  = std::min(room, button == 0 ? carried_.count : 1);
        if (put <= 0) {
            return;
        }
        const i32 now = (there.empty() ? 0 : there.count) + put;
        set(slot, carried_.with_count(now), effects);
        carried_.count -= put;
        if (carried_.count <= 0) {
            carried_ = SlotStack{};
        }
        return;
    }
    // Different items: swap, when what is in hand fits the slot.
    if (carried_.count <= limit) {
        SlotStack previous = there;
        set(slot, carried_, effects);
        carried_ = std::move(previous);
    }
}

void CreativeInventory::hotbar_key_on_slot(i16 slot, i32 hotbar, CreativeEffects& effects) {
    if (slot < 0 || static_cast<usize>(slot) >= kSlots || hotbar < 0 || hotbar > 8) {
        return;
    }
    const auto target = static_cast<i16>(kHotbarFirst + hotbar);
    if (target == slot) {
        return;
    }
    const SlotStack from = slots_[static_cast<usize>(slot)];
    const SlotStack to   = slots_[static_cast<usize>(target)];
    if ((!to.empty() && !may_place(slot, to)) || (!from.empty() && !may_place(target, from))) {
        return;
    }
    set(slot, to, effects);
    set(target, from, effects);
}

void CreativeInventory::throw_slot(i16 slot, bool whole_stack, CreativeEffects& effects) {
    if (slot < 0 || static_cast<usize>(slot) >= kSlots) {
        return;
    }
    const SlotStack there = slots_[static_cast<usize>(slot)];
    if (there.empty()) {
        return;
    }
    const i32 count = whole_stack ? there.count : 1;
    effects.drops.push_back(there.with_count(count));
    set(slot, there.with_count(there.count - count), effects);
}

// ── Everything else ─────────────────────────────────────────────────────────

void CreativeInventory::click_outside(i32 button, CreativeEffects& effects) {
    if (carried_.empty()) {
        return;
    }
    if (button == 1) {
        effects.drops.push_back(carried_.with_count(1));
        carried_.count -= 1;
        if (carried_.count <= 0) {
            carried_ = SlotStack{};
        }
        return;
    }
    effects.drops.push_back(carried_);
    carried_ = SlotStack{};
}

void CreativeInventory::click_destroy(bool shift, CreativeEffects& effects) {
    if (shift) {
        // Measured: every slot the survival page shows is emptied.
        for (usize slot = 5; slot < kSlots; ++slot) {
            set(static_cast<i16>(slot), SlotStack{}, effects);
        }
        return;
    }
    // The stack in hand stops existing. The server never had it.
    carried_ = SlotStack{};
}

}  // namespace ov::client
