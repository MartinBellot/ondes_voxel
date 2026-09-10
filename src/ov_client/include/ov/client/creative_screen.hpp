// The creative inventory: fourteen tabs, a page of forty-five cells, a
// scrollbar, a search field and a destroy slot.
//
// Two halves that must not be confused.
//
//   • **The catalogue** — the tabs and their cells — is data, loaded from
//     `render::CreativeTabs`. It is never invented here: in 1.20.1 the tabs are
//     built by the game's own code and the client never receives them, so the
//     order comes from running the shipped jar and asking. See
//     docs/provenance/inventaire-creatif.md.
//
//   • **The geometry** is measured from the pack's own textures by
//     scripts/measure_creative_tabs.py, the same way ContainerScreen's slots
//     were measured from `inventory.png`. `tab_items.png` yields 54 slot
//     squares — 45 item cells in a 9×5 grid plus the 9 of the hotbar — and
//     `tabs.png` yields seven 26-wide buttons in four bands.
//
// The link to the server is `Set Creative Slot`, and it is the one packet in
// the game the client is authoritative for: the server applies it in creative
// and *ignores it in survival*, which is vanilla's own behaviour. So this
// screen does move stacks by itself — and only this screen, and only in
// creative. Everything else still goes through Click Container and waits.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/item_view.hpp"
#include "ov/render/creative_tabs.hpp"
#include "ov/render/language.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

/// The measured geometry, in GUI pixels relative to the panel's top-left.
///
/// Every one of these is a fact about the pixels of
/// `gui/container/creative_inventory/*.png`, printed by
/// scripts/measure_creative_tabs.py. The two that are *derived* rather than
/// read say so.
namespace creative_layout {

inline constexpr f32 kPanelWidth  = 195.0F;
inline constexpr f32 kPanelHeight = 136.0F;

/// The 9×5 page: 45 cells whose top-left corners are (9 + 18c, 18 + 18r).
inline constexpr f32 kCellX     = 9.0F;
inline constexpr f32 kCellY     = 18.0F;
inline constexpr f32 kCellPitch = 18.0F;
inline constexpr f32 kCellSize  = 16.0F;
inline constexpr i32 kColumns   = 9;
inline constexpr i32 kRows      = 5;
inline constexpr i32 kPageCells = kColumns * kRows;

/// The hotbar row of the same page.
inline constexpr f32 kHotbarY = 112.0F;

/// The scrollbar's groove, read out of `tab_items.png`: the slot grey runs
/// x = 175..186, y = 18..127. The handle is 12×15, so its travel is 95.
inline constexpr f32 kScrollX      = 175.0F;
inline constexpr f32 kScrollY      = 18.0F;
inline constexpr f32 kScrollWidth  = 12.0F;
inline constexpr f32 kScrollHeight = 110.0F;
inline constexpr f32 kHandleHeight = 15.0F;

/// The search field's grey interior, found by the same square hunt that finds
/// the slots: one 16×16 square at (81, 5) in `tab_item_search.png`. The field
/// itself is wider; this is the one corner the detector can name.
inline constexpr f32 kSearchX      = 82.0F;
inline constexpr f32 kSearchY      = 6.0F;
inline constexpr f32 kSearchWidth  = 80.0F;
inline constexpr f32 kSearchHeight = 16.0F;

/// The destroy slot in the survival page: the only 16×16 square that is not
/// slot grey but pink, at (173, 112).
inline constexpr f32 kDestroyX = 173.0F;
inline constexpr f32 kDestroyY = 112.0F;

/// A tab button in the sheet: 26 wide, four bands 32 tall at y = 0 (top row,
/// unselected), 32 (top, selected), 64 (bottom, unselected), 96 (bottom,
/// selected). Seven per band, pitch 26.
inline constexpr f32 kTabSheetPitch = 26.0F;
inline constexpr f32 kTabWidth      = 26.0F;
inline constexpr f32 kTabHeight     = 32.0F;

/// **Derived, not read.** Seven buttons of 26 have to span a panel of 195, so
/// the on-screen pitch is (195 − 26) / 6 = 28.17 → 28: the last button then
/// ends at 194, one pixel inside the panel, and six two-pixel gaps separate
/// them. A pitch of 26 would leave thirteen pixels of panel bare on the right,
/// which is not what the sheet's own symmetry describes.
inline constexpr f32 kTabPitch = 28.0F;

/// **Derived, not read.** A top-row button hangs above the panel and a
/// bottom-row one below it, each overlapping by four pixels so the selected
/// one joins the panel's edge.
inline constexpr f32 kTabOverlap = 4.0F;

}  // namespace creative_layout

/// What the pointer is over.
enum class CreativeHit : u8 {
    None,
    /// A cell of the item page. `index` is 0..44 within the visible page.
    Cell,
    /// A slot of the player's inventory, drawn under the page. `index` is the
    /// window-0 slot number the protocol uses.
    PlayerSlot,
    /// A tab button. `index` indexes CreativeTabs::tabs().
    Tab,
    /// The scrollbar groove.
    Scrollbar,
    /// The destroy slot on the survival page.
    Destroy,
    /// The search field.
    SearchField,
};

struct CreativeTarget {
    CreativeHit kind{CreativeHit::None};
    i32         index{-1};
};

class CreativeScreen {
public:
    /// Borrows the catalogue and the language, which outlive the screen.
    CreativeScreen(const render::CreativeTabs& tabs, const render::Language& language);

    // ── State ───────────────────────────────────────────────────────────────

    [[nodiscard]] usize tab_count() const noexcept { return tabs_->tabs().size(); }

    [[nodiscard]] usize selected_tab() const noexcept { return selected_; }

    /// Select by index. Rebuilds the page and resets the scroll, as vanilla
    /// does — a tab remembered mid-scroll shows a different first row than the
    /// one the player left.
    void select(usize index);

    /// Select by registry id, e.g. `minecraft:redstone_blocks`. False when
    /// there is no such tab, which the caller must report rather than silently
    /// showing another.
    [[nodiscard]] bool select(std::string_view id);

    [[nodiscard]] const render::CreativeTab& tab() const noexcept {
        return tabs_->tabs()[selected_];
    }

    /// The cells of the current page after the search filter, in order.
    [[nodiscard]] std::span<const render::CreativeStack* const> page() const noexcept {
        return page_;
    }

    /// The first visible row, in rows of nine.
    [[nodiscard]] i32 scroll_row() const noexcept;

    /// How many rows the page could scroll past. Zero when it fits.
    [[nodiscard]] i32 scroll_range() const noexcept;

    [[nodiscard]] bool scrollable() const noexcept { return scroll_range() > 0; }

    /// Scroll by notches of the wheel. Vanilla's sign: away from the player
    /// scrolls up.
    void scroll_by(f32 notches);

    /// Put the handle where a pointer at this y (in GUI pixels, on the screen)
    /// says. Used while a drag is in progress.
    void drag_scroll(f32 screen_height, f32 mouse_y);

    [[nodiscard]] f32 scroll() const noexcept { return scroll_; }

    // ── The search field ────────────────────────────────────────────────────

    [[nodiscard]] bool searching() const noexcept {
        return tab().type == render::CreativeTabType::Search;
    }

    [[nodiscard]] const std::string& query() const noexcept { return query_; }

    /// Append typed text, or remove the last character. Both rebuild the page.
    void type(std::string_view utf8);
    void backspace();

    // ── Geometry ────────────────────────────────────────────────────────────

    [[nodiscard]] f32 origin_x(f32 screen_width) const noexcept;
    [[nodiscard]] f32 origin_y(f32 screen_height) const noexcept;

    /// What the pointer is over. Everything the screen reacts to goes through
    /// this one function, so a gesture and a scripted check cannot disagree.
    [[nodiscard]] CreativeTarget hit_test(f32 screen_width, f32 screen_height, f32 mouse_x,
                                          f32 mouse_y) const noexcept;

    /// The stack in a visible cell, or null when the page ends before it.
    [[nodiscard]] const render::CreativeStack* cell(i32 index) const noexcept;

    // ── Drawing ─────────────────────────────────────────────────────────────

    /// The background texture for the current tab, as a resource location.
    [[nodiscard]] std::string_view background_texture() const noexcept;

    /// `player` is the caller's mirror of window 0, in slot order — the hotbar
    /// is slots 36..44 and is what a category page shows along its bottom.
    void draw(Gui& gui, const ItemRenderer& items, GuiTexture background, GuiTexture tab_sheet,
              std::span<const ItemStackView> player, const CreativeTarget& hovered) const;

    /// The name under the pointer, for the tooltip line. Empty for none.
    [[nodiscard]] std::string_view hovered_name(const CreativeTarget& target,
                                                std::span<const ItemStackView> player) const;

private:
    void rebuild_page();

    /// Where a tab button's top-left lands on the screen.
    [[nodiscard]] GuiPoint tab_origin(f32 screen_width, f32 screen_height,
                                      const render::CreativeTab& tab) const noexcept;

    const render::CreativeTabs* tabs_{nullptr};
    const render::Language*     language_{nullptr};

    usize selected_{0};
    /// 0 at the top, 1 at the bottom, as vanilla keeps it.
    f32         scroll_{0.0F};
    std::string query_;

    /// Rebuilt on a tab change or a keystroke, never inside draw().
    std::vector<const render::CreativeStack*> page_;
    /// Lower-cased translated names, parallel to the current tab's stacks.
    /// Built once per tab rather than once per keystroke: the search page has
    /// 1587 of them.
    std::vector<std::string> names_;
    /// Scratch for the filter, kept so a keystroke does not allocate.
    std::string folded_;
};

}  // namespace ov::client
