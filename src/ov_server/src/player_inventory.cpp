#include "player_inventory.hpp"

#include <algorithm>
#include <utility>

namespace ov::server {
namespace {

[[nodiscard]] i8 stack_limit(const registry::Registries* registries, i32 item_id) {
    return registries != nullptr ? registries->max_stack_size(item_id) : i8{64};
}

[[nodiscard]] gameplay::RecipeStack to_recipe(const net::ItemStack& stack) {
    return stack.empty() ? gameplay::RecipeStack{}
                         : gameplay::RecipeStack{static_cast<registry::ProtocolId>(stack.item_id),
                                                 static_cast<i32>(stack.count)};
}

[[nodiscard]] net::ItemStack from_recipe(const gameplay::RecipeStack& stack) {
    if (stack.empty()) {
        return {};
    }
    return net::ItemStack{static_cast<i32>(stack.item), static_cast<i8>(std::min(stack.count, 127)),
                          {}};
}

/// The 2x2 as `ov_gameplay` wants it.
///
/// Row-major, and the grid's own `at()` indexes by `width`, so the four cells
/// of a 2x2 are 0,1,2,3 and not 0,1,3,4. Filling it as if it were a 3x3 with
/// two empty columns is the mistake that makes a 2x2 recipe never match.
[[nodiscard]] gameplay::CraftingGrid grid_of(std::span<const net::ItemStack> inventory) {
    gameplay::CraftingGrid grid;
    grid.width  = 2;
    grid.height = 2;
    for (usize i = 0; i < 4; ++i) {
        grid.cells[i] = to_recipe(inventory[static_cast<usize>(kCraftGridFirst) + i]);
    }
    return grid;
}

/// Put a stack somewhere in a half-open range, merging first and only then
/// filling an empty slot.
///
/// Merging before filling is what vanilla does and what makes a shift-click
/// feel like one move rather than scattering a stack across free slots.
void push_into(std::span<net::ItemStack> inventory, net::ItemStack& from, i16 first, i16 last,
               const registry::Registries* registries) {
    if (from.empty()) {
        return;
    }
    const i8 limit = stack_limit(registries, from.item_id);
    for (i16 index = first; index < last && !from.empty(); ++index) {
        net::ItemStack& into = inventory[static_cast<usize>(index)];
        if (into.empty() || into.item_id != from.item_id || into.count >= limit) {
            continue;
        }
        const i8 moved = std::min(static_cast<i8>(limit - into.count), from.count);
        into.count     = static_cast<i8>(into.count + moved);
        from.count     = static_cast<i8>(from.count - moved);
        if (from.count <= 0) {
            from = {};
        }
    }
    for (i16 index = first; index < last && !from.empty(); ++index) {
        net::ItemStack& into = inventory[static_cast<usize>(index)];
        if (into.empty()) {
            into = std::exchange(from, net::ItemStack{});
        }
    }
}

/// Where a shift-click sends a stack, in the order vanilla tries.
///
/// Not one range but a list, because the player's window has three halves and
/// not two: the hotbar and the backpack trade with each other, and everything
/// else — armour, the grid, the off hand — empties into the backpack first and
/// the hotbar after.
void shift_move(std::span<net::ItemStack> inventory, i16 slot,
                const registry::Registries* registries) {
    net::ItemStack& from = inventory[static_cast<usize>(slot)];
    if (from.empty()) {
        return;
    }
    if (slot >= kHotbarFirst && slot < kOffhandSlot) {
        push_into(inventory, from, kBackpackFirst, kHotbarFirst, registries);
        return;
    }
    if (slot >= kBackpackFirst && slot < kHotbarFirst) {
        push_into(inventory, from, kHotbarFirst, kOffhandSlot, registries);
        return;
    }
    // The grid, the armour and the off hand all unload into the player's
    // storage, backpack first.
    push_into(inventory, from, kBackpackFirst, kHotbarFirst, registries);
    push_into(inventory, from, kHotbarFirst, kOffhandSlot, registries);
}

}  // namespace

net::ItemStack player_craft_result(const registry::Registries*     registries,
                                   const gameplay::RecipeBook*     book,
                                   std::span<const net::ItemStack> inventory) {
    (void)registries;
    if (book == nullptr || inventory.size() < kPlayerWindowSlots) {
        return {};
    }
    const auto match = gameplay::match_crafting(*book, grid_of(inventory));
    return match ? from_recipe(match->result) : net::ItemStack{};
}

std::vector<net::ItemStack> player_window_contents(const registry::Registries* registries,
                                                   const gameplay::RecipeBook* book,
                                                   std::span<const net::ItemStack> inventory) {
    std::vector<net::ItemStack> slots;
    slots.reserve(kPlayerWindowSlots);
    slots.push_back(player_craft_result(registries, book, inventory));
    for (usize i = 1; i < kPlayerWindowSlots; ++i) {
        slots.push_back(inventory[i]);
    }
    return slots;
}

PlayerClickOutcome apply_player_click(const registry::Registries* registries,
                                      const gameplay::RecipeBook* book,
                                      const net::ContainerClick& click,
                                      std::span<net::ItemStack> inventory, net::ItemStack& carried,
                                      DragState& drag) {
    PlayerClickOutcome outcome;
    if (inventory.size() < kPlayerWindowSlots) {
        return outcome;
    }

    const auto in_range = [&](i16 index) {
        return index >= 0 && index < static_cast<i16>(kPlayerWindowSlots);
    };

    /// A slot a click may put something *into*. Slot 0 is not one: it shows the
    /// craft result and cannot be written, and treating it as storage is how a
    /// player duplicates whatever they last made.
    const auto writable = [&](i16 index) -> net::ItemStack* {
        return in_range(index) && index != kCraftResultSlot
                   ? &inventory[static_cast<usize>(index)]
                   : nullptr;
    };

    // ── Taking the craft result ─────────────────────────────────────────────
    //
    // Handled before everything else because it is the one slot whose contents
    // are computed rather than stored, and because every mode reaches it
    // differently while all of them mean the same thing: take one craft.
    if (click.slot == kCraftResultSlot && book != nullptr && click.mode != 5) {
        const gameplay::CraftingGrid grid  = grid_of(inventory);
        const auto                   match = gameplay::match_crafting(*book, grid);
        if (!match) {
            outcome.handled = true;
            return outcome;
        }
        const net::ItemStack result = from_recipe(match->result);
        const i8             limit  = stack_limit(registries, result.item_id);

        // How many times the player is allowed to take one. A shift-click
        // crafts in a loop until the grid runs out — the one click in the game
        // that does an unbounded amount of work, so the bound is explicit.
        i32 wanted = 1;
        if (click.mode == 1) {
            wanted = gameplay::max_crafts(*book, grid, *match);
        } else if (!carried.empty()) {
            // An ordinary click only works onto an empty cursor or onto more of
            // the same, and only while there is room.
            if (carried.item_id != result.item_id ||
                carried.count + result.count > limit) {
                outcome.handled = true;
                return outcome;
            }
        }

        gameplay::CraftingGrid working = grid;
        for (i32 taken = 0; taken < wanted; ++taken) {
            const auto step = gameplay::match_crafting(*book, working);
            if (!step) {
                break;
            }
            const net::ItemStack made = from_recipe(step->result);
            if (click.mode == 1) {
                net::ItemStack into = made;
                push_into(inventory, into, kBackpackFirst, kOffhandSlot, registries);
                if (!into.empty()) {
                    // Nowhere left to put it: stop rather than destroy it.
                    break;
                }
            } else {
                if (carried.empty()) {
                    carried = made;
                } else if (carried.count + made.count <= limit) {
                    carried.count = static_cast<i8>(carried.count + made.count);
                } else {
                    break;
                }
            }

            const gameplay::CraftConsumption after =
                gameplay::consume_craft(*book, working, *step);
            working = after.grid;
            for (usize i = 0; i < after.overflow_count; ++i) {
                net::ItemStack spare = from_recipe(after.overflow[i]);
                push_into(inventory, spare, kBackpackFirst, kOffhandSlot, registries);
                if (!spare.empty()) {
                    outcome.dropped.push_back(std::move(spare));
                }
            }
            if (click.mode != 1) {
                break;
            }
        }

        for (usize i = 0; i < 4; ++i) {
            inventory[static_cast<usize>(kCraftGridFirst) + i] = from_recipe(working.cells[i]);
        }
        outcome.handled = true;
        return outcome;
    }

    switch (click.mode) {
        // ── Pick up and put down ────────────────────────────────────────────
        case 0: {
            net::ItemStack* slot = writable(click.slot);
            if (slot == nullptr) {
                break;
            }
            if (click.button == 0) {
                const i8 limit = stack_limit(registries, carried.item_id);
                if (!carried.empty() && !slot->empty() && slot->item_id == carried.item_id &&
                    slot->count < limit) {
                    const i8 moved =
                        std::min(static_cast<i8>(limit - slot->count), carried.count);
                    slot->count   = static_cast<i8>(slot->count + moved);
                    carried.count = static_cast<i8>(carried.count - moved);
                    if (carried.count <= 0) {
                        carried = {};
                    }
                } else {
                    std::swap(*slot, carried);
                }
                outcome.handled = true;
            } else if (click.button == 1) {
                if (carried.empty()) {
                    if (!slot->empty()) {
                        // Rounding **up** on the half is what vanilla does;
                        // rounding down loses an item on every odd stack.
                        const i8 half = static_cast<i8>((slot->count + 1) / 2);
                        carried       = *slot;
                        carried.count = half;
                        slot->count   = static_cast<i8>(slot->count - half);
                        if (slot->count <= 0) {
                            *slot = {};
                        }
                    }
                } else {
                    const i8   limit = stack_limit(registries, carried.item_id);
                    const bool same  = !slot->empty() && slot->item_id == carried.item_id;
                    if (slot->empty() || (same && slot->count < limit)) {
                        if (slot->empty()) {
                            *slot       = carried;
                            slot->count = 1;
                        } else {
                            slot->count = static_cast<i8>(slot->count + 1);
                        }
                        carried.count = static_cast<i8>(carried.count - 1);
                        if (carried.count <= 0) {
                            carried = {};
                        }
                    }
                }
                outcome.handled = true;
            }
            break;
        }

        // ── Shift-click ─────────────────────────────────────────────────────
        case 1: {
            if (writable(click.slot) != nullptr) {
                shift_move(inventory, click.slot, registries);
                outcome.handled = true;
            }
            break;
        }

        // ── Number keys ─────────────────────────────────────────────────────
        case 2: {
            if (click.button < 0 || click.button > 8) {
                break;
            }
            net::ItemStack* slot   = writable(click.slot);
            net::ItemStack* hotbar = &inventory[static_cast<usize>(kHotbarFirst + click.button)];
            if (slot != nullptr && slot != hotbar) {
                std::swap(*slot, *hotbar);
            }
            outcome.handled = true;
            break;
        }

        // ── Throwing ────────────────────────────────────────────────────────
        case 4: {
            net::ItemStack* slot = click.slot == -999 ? &carried : writable(click.slot);
            if (slot != nullptr && !slot->empty()) {
                // Button 1 throws the whole stack, button 0 a single item.
                const i8 amount = click.button == 1 ? slot->count : i8{1};
                net::ItemStack thrown = *slot;
                thrown.count          = amount;
                slot->count           = static_cast<i8>(slot->count - amount);
                if (slot->count <= 0) {
                    *slot = {};
                }
                outcome.dropped.push_back(std::move(thrown));
            }
            outcome.handled = true;
            break;
        }

        // ── Dragging, which vanilla calls painting ──────────────────────────
        case 5: {
            const auto phase = static_cast<i8>(click.button % 4);
            if (phase == 0) {
                drag.slots.clear();
                drag.button = click.button;
            } else if (phase == 1) {
                if (drag.button >= 0 &&
                    std::ranges::find(drag.slots, click.slot) == drag.slots.end()) {
                    drag.slots.push_back(click.slot);
                }
            } else if (phase == 2 && drag.button >= 0) {
                // Button 0-3 is a left drag and splits evenly, keeping the
                // remainder on the cursor; 4-7 is a right drag and puts one in
                // each. Slots already holding something else are skipped rather
                // than overwritten.
                const bool one_each = drag.button >= 4;
                const i8   limit    = stack_limit(registries, carried.item_id);

                std::vector<net::ItemStack*> targets;
                for (const i16 index : drag.slots) {
                    net::ItemStack* slot = writable(index);
                    if (slot == nullptr || carried.empty()) {
                        continue;
                    }
                    if (slot->empty() ||
                        (slot->item_id == carried.item_id && slot->count < limit)) {
                        targets.push_back(slot);
                    }
                }
                if (!targets.empty() && !carried.empty()) {
                    const i8 share = one_each ? i8{1}
                                              : static_cast<i8>(carried.count /
                                                                static_cast<i8>(targets.size()));
                    for (net::ItemStack* slot : targets) {
                        if (carried.count <= 0 || share <= 0) {
                            break;
                        }
                        const i8 room  = static_cast<i8>(limit - (slot->empty() ? 0 : slot->count));
                        const i8 moved = std::min({share, room, carried.count});
                        if (moved <= 0) {
                            continue;
                        }
                        if (slot->empty()) {
                            *slot       = carried;
                            slot->count = moved;
                        } else {
                            slot->count = static_cast<i8>(slot->count + moved);
                        }
                        carried.count = static_cast<i8>(carried.count - moved);
                    }
                    if (carried.count <= 0) {
                        carried = {};
                    }
                }
                drag.slots.clear();
                drag.button = -1;
            }
            outcome.handled = true;
            break;
        }

        default:
            // Mode 3 is creative middle-click, and the client resolves it on
            // its own and tells us through `Set Creative Slot`. Nothing else
            // exists in 1.20.1. An unknown mode is left unhandled rather than
            // guessed, and the caller resends the window, which puts the
            // client back in step.
            break;
    }

    return outcome;
}

}  // namespace ov::server
