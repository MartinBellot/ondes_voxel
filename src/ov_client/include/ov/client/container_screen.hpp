// The windows: the player's own inventory, a chest, a crafting table, a
// furnace.
//
// A screen here is three things and nothing else — a background to blit, a
// list of slot rectangles, and a rule for turning a click on one of them into
// a Click Container packet. It owns no items. The slot *contents* live in the
// caller's mirror of what the server last sent, and are handed in for drawing;
// the screen never writes them.
//
// That is the whole design, and it is the mandate's point. The server applies
// the six click modes authoritatively and has been measured doing it. A screen
// that moved a stack locally and then reconciled would be right ninety-nine
// times and wrong the hundredth, and the hundredth is the one where an item
// disappears. So: a click is sent, nothing moves, and the next Set Container
// Slot moves it.
//
// The layouts are **measured from the pack's own textures**, not remembered:
// scripts/measure_gui_sprites.py finds every 16×16 slot interior in
// `inventory.png` and `generic_54.png` and prints its corner. See
// docs/provenance/interface.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/item_view.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::client {

/// The shapes of window this client can draw.
///
/// A menu type that is not one of these is **refused and named** rather than
/// opened as a chest: a furnace drawn with a chest's slot list would put the
/// fuel where the player expects their own inventory, and clicking it would
/// send the server a slot number that means something else entirely.
enum class ScreenKind : u8 {
    /// Window 0. Not opened by the server: the client puts it up itself.
    PlayerInventory,
    /// generic_9x1 .. generic_9x6, menu types 0..5.
    Chest,
    /// menu type 11.
    CraftingTable,
    /// menu types 9, 13 and 21 — blast furnace, furnace, smoker. The same
    /// three slots and the same background, different textures.
    Furnace,
    // ── hud ── the windows the server opens and this client did not draw,
    // laid out from their textures' own slot squares (docs/provenance/hud.md).
    /// generic_3x3 (6): dispenser and dropper.
    Dispenser,
    /// hopper (15): five in a row, a window 133 high.
    Hopper,
    /// shulker_box (19): three rows of nine, its own texture.
    ShulkerBox,
    /// anvil (7): two inputs and a result.
    Anvil,
    /// grindstone (14): two inputs and a result.
    Grindstone,
    /// enchantment (12): the item and the lapis.
    Enchanting,
    /// brewing_stand (10): three bottles, the ingredient, the blaze powder.
    BrewingStand,
    /// Open Horse Screen, not a menu type: saddle, armour, and a chest of
    /// three rows when the animal carries one.
    Horse,
};

/// One slot: where it is, and what number the server calls it.
struct SlotRect {
    /// The slot index inside the window, exactly as Click Container carries it.
    i16 index{0};
    /// The cell's top-left, relative to the window's, in GUI pixels.
    f32 x{0.0F};
    f32 y{0.0F};
    /// ── hud ── On the wire but not on the screen: a donkey's armour slot, a
    /// llama's saddle. Neither drawn nor clickable, as in vanilla.
    bool hidden{false};
};

/// ── hud ── What a horse-like window shows besides its chest, by animal
/// (measured on the real client: 39–41-*-inventory).
struct HorseParts {
    /// Horse, donkey, mule, skeleton and zombie horses, camel — not a llama.
    bool saddle{true};
    /// 0 none (donkey, mule…), 1 horse armour, 2 a llama's carpet.
    i32 armour{1};
};

/// What a click on a screen means, in the protocol's own terms.
struct ClickIntent {
    i16 slot{0};
    i8  button{0};
    i32 mode{0};
};

/// The six modes, named. Their numbers are the protocol's.
namespace click_mode {
inline constexpr i32 kPickup      = 0;
inline constexpr i32 kQuickMove   = 1;  // shift-click
inline constexpr i32 kSwap        = 2;  // a number key
inline constexpr i32 kClone       = 3;  // middle click, creative only
inline constexpr i32 kThrow       = 4;  // Q, or a click outside with a full cursor
inline constexpr i32 kQuickCraft  = 5;  // a drag
inline constexpr i32 kPickupAll   = 6;  // double click
}  // namespace click_mode

/// A window's geometry and its slot list.
class ContainerScreen {
public:
    /// The player's own inventory: window 0, 46 slots.
    [[nodiscard]] static ContainerScreen player_inventory();

    /// Build from an Open Screen packet. Nullopt for a menu type this client
    /// does not draw — the caller must then close the window rather than show
    /// the wrong one.
    [[nodiscard]] static std::optional<ContainerScreen> from_menu(i32 menu_type, u8 window_id,
                                                                  std::string title);

    /// ── hud ── Build from an Open Horse Screen: `container_slots` is the
    /// packet's slot count — 2 for a horse, 2 + 3 × columns with a chest.
    [[nodiscard]] static ContainerScreen from_horse(u8 window_id, i32 container_slots,
                                                    std::string title, HorseParts parts = {});

    /// Chest columns of a horse-like window; 0 without a chest.
    [[nodiscard]] i32 horse_columns() const noexcept { return horse_columns_; }

    [[nodiscard]] ScreenKind kind() const noexcept { return kind_; }
    [[nodiscard]] u8         window_id() const noexcept { return window_id_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] usize      slot_count() const noexcept { return slots_.size(); }
    [[nodiscard]] const std::vector<SlotRect>& slots() const noexcept { return slots_; }

    /// The window's size in GUI pixels.
    [[nodiscard]] f32 width() const noexcept { return width_; }
    [[nodiscard]] f32 height() const noexcept { return height_; }

    /// The window's top-left on a screen of this size, centred as vanilla
    /// centres it: horizontally on the middle, vertically on the middle.
    [[nodiscard]] f32 origin_x(f32 screen_width) const noexcept;
    [[nodiscard]] f32 origin_y(f32 screen_height) const noexcept;

    /// Which slot the cursor is over, in GUI pixels on the screen. Nullopt for
    /// none — which is not the same as slot −999, and the caller decides.
    [[nodiscard]] const SlotRect* slot_at(f32 screen_width, f32 screen_height, f32 mouse_x,
                                          f32 mouse_y) const noexcept;

    /// Turn a mouse or key event over a slot into what to send.
    ///
    /// `button`: 0 left, 1 right, 2 middle. `shift` is the modifier.
    /// `hotbar_key` is 0..8 for the number keys, −1 for none.
    [[nodiscard]] static ClickIntent click(i16 slot, i32 button, bool shift,
                                           i32 hotbar_key) noexcept;

    /// Draw the background and every slot's item.
    ///
    /// `contents` is the caller's mirror of the window, in slot order. Shorter
    /// than the slot list is tolerated and draws what there is: a window whose
    /// content packet has not arrived yet is empty, not wrong.
    void draw(Gui& gui, const ItemRenderer& items, GuiTexture background,
              std::span<const ItemStackView> contents, const SlotRect* hovered) const;

    /// The texture this screen's background lives in, as a resource location.
    [[nodiscard]] std::string_view background_texture() const noexcept;

private:
    ContainerScreen() = default;

    ScreenKind            kind_{ScreenKind::PlayerInventory};
    u8                    window_id_{0};
    std::string           title_;
    std::vector<SlotRect> slots_;
    f32                   width_{176.0F};
    f32                   height_{166.0F};
    /// Chest rows, for the two-part background blit.
    i32 rows_{0};
    i32        horse_columns_{0};  // ── hud ──
    HorseParts horse_parts_{};
};

/// Vanilla's dim behind an open screen: a vertical gradient from 0x10101010 to
/// 0xC0101010, drawn over the whole viewport.
void draw_screen_dim(Gui& gui);

}  // namespace ov::client
