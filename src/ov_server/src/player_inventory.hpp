// The player's own screen: window 0.
//
// Every other window this server knows how to open — a chest, a crafting table,
// a furnace — is opened by the server, given an id and remembered. Window 0 is
// the one the client opens **by itself**, when the player presses E. The server
// is never told, sends no `Open Screen`, and gets clicks on a window it has no
// record of.
//
// The old handler required `window_open` and matched `window_id`, so every one
// of those clicks was dropped on the floor. A player could rearrange their
// inventory on screen, watch the server resend nothing, and see the change
// vanish the moment anything else refreshed the window. Moving a single stack
// from the hotbar to the backpack was impossible.
//
// ── The slot numbering ─────────────────────────────────────────────────────
//
// Window 0 has 46 slots and `Player::inventory` already stores exactly those,
// in exactly that order, so the mapping here is the identity — which is
// precisely why the bug survived: every *other* window needs a translation, and
// the one that needs none was the one that had no code at all.
//
//     0        the 2x2 crafting result — take-only
//     1..4     the 2x2 crafting grid
//     5..8     armour: head, chest, legs, feet
//     9..35    the backpack
//     36..44   the hotbar
//     45       the off hand
//
// Slot 0 is the only one that is not storage. It holds no item: it *shows* what
// the grid currently makes, and taking from it consumes the grid. The rules for
// that live in `ov_gameplay` and are the same ones the crafting table uses; the
// 2x2 differs from the 3x3 in two numbers and nothing else.
#pragma once

#include "ov/gameplay/crafting.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <span>
#include <vector>

namespace ov::server {

/// The player's window, and only that.
inline constexpr usize kPlayerWindowSlots = 46;
inline constexpr i16   kCraftResultSlot   = 0;
inline constexpr i16   kCraftGridFirst    = 1;
inline constexpr i16   kCraftGridLast     = 4;
inline constexpr i16   kArmourFirst       = 5;
inline constexpr i16   kBackpackFirst     = 9;
inline constexpr i16   kHotbarFirst       = 36;
inline constexpr i16   kOffhandSlot       = 45;

/// The drag ("painting") a player has started but not finished.
///
/// Three packets — start, each slot crossed, end — so the state has to survive
/// between them, and it belongs to the window rather than to one click.
struct DragState {
    std::vector<i16> slots;
    i8               button{-1};
};

/// What a click did that the caller has to carry out.
struct PlayerClickOutcome {
    bool handled{false};
    /// Stacks that must be thrown on the ground: a mode-4 drop, or a craft
    /// remainder with nowhere to go.
    std::vector<net::ItemStack> dropped;
};

/// Apply one click to the player's own window.
///
/// `inventory` is the player's 46 slots as the protocol numbers them, and
/// `carried` is what the cursor holds. Both are changed in place.
///
/// `book` may be null, in which case slot 0 stays empty and a click on it does
/// nothing — refused rather than silently crafting air.
///
/// `selected` is the held hotbar slot, 0..8: a craft's remainder that does not
/// fit back in the grid goes where vanilla's `Inventory.add` puts it, and that
/// tries the held slot first.
///
/// Measured against the real server (`scripts/measure_window0.py`,
/// docs/provenance/crafting-and-smelting.md): every mode on the result slot,
/// the double click, and where a shift-click sends armour and a shield.
[[nodiscard]] PlayerClickOutcome apply_player_click(const registry::Registries*  registries,
                                                    const gameplay::RecipeBook*  book,
                                                    const net::ContainerClick&   click,
                                                    std::span<net::ItemStack>    inventory,
                                                    net::ItemStack&              carried,
                                                    DragState&                   drag,
                                                    i16                          selected = 0);

/// The player closed window 0: the cursor and then the four grid cells go back
/// into the inventory — measured, in that order, first free slot hotbar
/// first. Returns what did not fit, to be thrown at the player's feet.
[[nodiscard]] std::vector<net::ItemStack> close_player_window(
    const registry::Registries* registries, std::span<net::ItemStack> inventory,
    net::ItemStack& carried, i16 selected);

/// Vanilla's `Inventory.add`: into a stack of the same item with room — the
/// held slot, then the off hand, then the storage in order — and only then the
/// first empty storage slot, hotbar first. Returns what did not fit.
[[nodiscard]] net::ItemStack add_to_inventory(const registry::Registries* registries,
                                              std::span<net::ItemStack>   inventory,
                                              net::ItemStack stack, i16 selected);

/// The window as one list, for `Set Container Content`.
///
/// Slot 0 is recomputed rather than read: it holds nothing of its own, and
/// sending whatever happens to be in that array cell would show the player the
/// last thing they crafted for ever.
[[nodiscard]] std::vector<net::ItemStack> player_window_contents(
    const registry::Registries* registries, const gameplay::RecipeBook* book,
    std::span<const net::ItemStack> inventory);

/// What the 2x2 grid currently makes, or an empty stack.
[[nodiscard]] net::ItemStack player_craft_result(const registry::Registries* registries,
                                                 const gameplay::RecipeBook* book,
                                                 std::span<const net::ItemStack> inventory);

}  // namespace ov::server
