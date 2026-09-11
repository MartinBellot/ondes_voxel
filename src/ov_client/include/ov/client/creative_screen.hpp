// The creative inventory: thirteen tabs (fourteen for an operator who asks), a
// page of forty-five cells, a scrollbar, a search field, the saved hotbars,
// and the survival page with its destroy slot.
//
// Three sources, and none of them is a recollection:
//
//   • **The catalogue** — tabs and cells — is render::CreativeTabs, produced
//     by running the server jar's own CreativeModeTabs
//     (scripts/measure_creative_tabs.py).
//   • **What a stack says** — its tooltip, its tints, its durability — is
//     render::CreativeItems, produced by running the user's own vanilla
//     1.20.1 client and asking it (scripts/measure_creative_screen.py).
//   • **The geometry** is counted in the pack's pixels and, since the second
//     pass, read off the running client: where the panel is, where each tab
//     button is, where each slot of each page is. Every constant below says
//     which.
//
// What a gesture *does* lives in creative_gestures.hpp; this file decides
// only where the pointer is and what the page shows.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/item_view.hpp"
#include "ov/client/saved_hotbars.hpp"
#include "ov/client/text_field.hpp"
#include "ov/render/creative_items.hpp"
#include "ov/render/creative_tabs.hpp"
#include "ov/render/item_model.hpp"
#include "ov/render/language.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::client {

/// The geometry, in GUI pixels relative to the panel's top-left.
namespace creative_layout {

/// Measured on the running client: imageWidth × imageHeight.
inline constexpr f32 kPanelWidth  = 195.0F;
inline constexpr f32 kPanelHeight = 136.0F;

/// The 9×5 page: 45 cells whose top-left corners are (9 + 18c, 18 + 18r) —
/// counted in tab_items.png and confirmed slot by slot on the running client.
inline constexpr f32 kCellX     = 9.0F;
inline constexpr f32 kCellY     = 18.0F;
inline constexpr f32 kCellPitch = 18.0F;
inline constexpr f32 kCellSize  = 16.0F;
inline constexpr i32 kColumns   = 9;
inline constexpr i32 kRows      = 5;
inline constexpr i32 kPageCells = kColumns * kRows;

/// The hotbar row: slots at y = 112 on every page (running client).
inline constexpr f32 kHotbarY = 112.0F;

/// The title, where the running client reports titleLabelX/Y.
inline constexpr f32 kTitleX = 8.0F;
inline constexpr f32 kTitleY = 6.0F;

/// The scrollbar's groove, read out of `tab_items.png`: x = 175..186,
/// y = 18..127. The handle is 12×15, so its travel is 95.
inline constexpr f32 kScrollX      = 175.0F;
inline constexpr f32 kScrollY      = 18.0F;
inline constexpr f32 kScrollWidth  = 12.0F;
inline constexpr f32 kScrollHeight = 110.0F;
inline constexpr f32 kHandleHeight = 15.0F;

/// The search box, as the running client reports it: (82, 6), 80×9. Its text
/// is drawn at the box's own origin — it has no border — in white, and it
/// takes at most 50 characters.
inline constexpr f32   kSearchX         = 82.0F;
inline constexpr f32   kSearchY         = 6.0F;
inline constexpr f32   kSearchWidth     = 80.0F;
inline constexpr f32   kSearchHeight    = 9.0F;
inline constexpr usize kSearchMaxLength = 50;

/// The survival page, slot by slot as the running client lays it out.
inline constexpr f32 kDestroyX = 173.0F;
inline constexpr f32 kDestroyY = 112.0F;
/// Window-0 slot 5 (head) … 8 (feet), then the off hand (45).
inline constexpr std::array<std::array<f32, 2>, 4> kArmour{{{54.0F, 6.0F},
                                                           {54.0F, 33.0F},
                                                           {108.0F, 6.0F},
                                                           {108.0F, 33.0F}}};
inline constexpr std::array<f32, 2> kOffHand{35.0F, 20.0F};
/// The three rows of the inventory: y = 54, 72, 90.
inline constexpr f32 kInventoryY = 54.0F;

/// A tab button in the sheet: 26 wide, four bands 32 tall at y = 0 (top row,
/// unselected), 32 (top, selected), 64 (bottom, unselected), 96 (bottom,
/// selected). Seven per band, pitch 26.
inline constexpr f32 kTabSheetPitch = 26.0F;
inline constexpr f32 kTabWidth      = 26.0F;
inline constexpr f32 kTabHeight     = 32.0F;

/// **Measured on the running 1.20.1 client** (scripts/measure_creative_screen.py
/// asks the game where each button is): a tab's x is 27 × column, and a
/// right-aligned tab — Saved Hotbars, Search, Survival Inventory — is anchored
/// from the panel's right edge instead, at 195 − 27 × (7 − column) + 1. So
/// Saved Hotbars is at 142 and not 135, and a seven-pixel gap separates it from
/// Redstone Blocks. The value this replaced, 28, had been *derived* from the
/// sheet and was one pixel per column wrong.
inline constexpr f32 kTabPitch = 27.0F;

[[nodiscard]] constexpr f32 tab_x(i32 column, bool aligned_right) noexcept {
    return aligned_right ? kPanelWidth - kTabPitch * static_cast<f32>(7 - column) + 1.0F
                         : kTabPitch * static_cast<f32>(column);
}

/// Where a button *reacts*: the running client's getTabY, −32 above the
/// panel for the top row and 136 below it for the bottom row, 32 tall.
inline constexpr f32 kTopTabHitY    = -32.0F;
inline constexpr f32 kBottomTabHitY = 136.0F;

/// Where a button is *drawn*, read off the running client's pixels: the top
/// row at −28 (it overlaps the panel by four), the bottom row at 132.
inline constexpr f32 kTopTabDrawY    = -28.0F;
inline constexpr f32 kBottomTabDrawY = 132.0F;

/// The icon inside a button, from the same pixels: five in, and nine below a
/// top button's edge. The bottom row's offset is the value that makes our
/// capture match the client's pixel for pixel; see the provenance document.
inline constexpr f32 kTabIconX       = 5.0F;
inline constexpr f32 kTopTabIconY    = 9.0F;
inline constexpr f32 kBottomTabIconY = 7.0F;

}  // namespace creative_layout

/// What the pointer is over.
enum class CreativeHit : u8 {
    None,
    /// A cell of the page. `index` is 0..44 within the visible page.
    Cell,
    /// A slot of the player's inventory. `index` is the window-0 slot.
    PlayerSlot,
    /// A tab button. `index` indexes CreativeScreen::tabs().
    Tab,
    Scrollbar,
    Destroy,
    SearchField,
};

struct CreativeTarget {
    CreativeHit kind{CreativeHit::None};
    i32         index{-1};
};

/// One cell of a page.
struct CreativeCell {
    std::string_view    item;
    i32                 count{1};
    std::span<const u8> nbt;
    /// What the running client says about it, or null.
    const render::CreativeItemInfo* info{nullptr};
    /// The saved-hotbar page's hint: shown, described, never taken.
    bool hint{false};
    /// For a hint, its tooltip line.
    std::string_view hint_line;
};

struct CreativeScreenOptions {
    /// Vanilla's "Operator Items Tab" setting. Off by default, as in vanilla,
    /// and then the operator tab and every stack only it holds are absent —
    /// from the search page too. The running client's search tab has 1557
    /// stacks, not the catalogue's 1586, for exactly that reason.
    bool operator_tab{false};
};

/// The textures the screen draws from, registered with the Gui by the caller.
struct CreativeTextures {
    GuiTexture background{GuiTexture::Invalid};
    GuiTexture tabs{GuiTexture::Invalid};
    /// The empty-slot silhouettes: helmet, chestplate, leggings, boots,
    /// shield. Atlas sprites; absent ones are simply not drawn.
    std::array<std::optional<render::SpriteUv>, 5> slot_icons{};
};

class CreativeScreen {
public:
    CreativeScreen(const render::CreativeTabs& tabs, const render::Language& language,
                   const render::CreativeItems* items = nullptr,
                   CreativeScreenOptions        options = {});

    // ── Tabs ────────────────────────────────────────────────────────────────

    /// The tabs shown, in catalogue order.
    [[nodiscard]] std::span<const render::CreativeTab* const> tabs() const noexcept {
        return visible_;
    }

    [[nodiscard]] usize tab_count() const noexcept { return visible_.size(); }

    /// ── allow-commands ── Show or hide the operator tab (and the stacks only
    /// it holds) after construction: vanilla's client decides it from the
    /// "Operator Items Tab" option *and* the player's permission level, which
    /// the server can change at any time. The selected tab is kept when it is
    /// still shown, else Building Blocks.
    void set_operator_tab(bool shown);
    [[nodiscard]] bool operator_tab() const noexcept { return options_.operator_tab; }

    [[nodiscard]] usize selected_tab() const noexcept { return selected_; }

    [[nodiscard]] const render::CreativeTab& tab() const noexcept { return *visible_[selected_]; }

    /// Select by index into tabs(). Rebuilds the page and resets the scroll.
    void select(usize index);

    /// Select by registry id. False when there is no such tab shown.
    [[nodiscard]] bool select(std::string_view id);

    // ── The page ────────────────────────────────────────────────────────────

    [[nodiscard]] std::span<const CreativeCell> page() const noexcept { return page_; }

    [[nodiscard]] i32 scroll_row() const noexcept;
    [[nodiscard]] i32 scroll_range() const noexcept;
    [[nodiscard]] bool scrollable() const noexcept { return scroll_range() > 0; }
    [[nodiscard]] f32 scroll() const noexcept { return scroll_; }

    /// Scroll by wheel notches; away from the player scrolls up.
    void scroll_by(f32 notches);

    /// Put the handle where a pointer at this y says, while a drag holds.
    void drag_scroll(f32 screen_height, f32 mouse_y);

    /// The cell at a visible index, or null.
    [[nodiscard]] const CreativeCell* cell(i32 index) const noexcept;

    // ── The saved hotbars ───────────────────────────────────────────────────

    /// Borrow the saved rows, and the key labels the hint names (the save
    /// key, then the nine number keys as the keyboard layout prints them).
    void set_saved_hotbars(const SavedHotbars* hotbars, std::string_view save_key,
                           const std::array<std::string, 9>& hotbar_keys);

    /// Rebuild the saved-hotbar page after a row changed.
    void refresh_saved_hotbars();

    // ── The search ──────────────────────────────────────────────────────────

    [[nodiscard]] bool searching() const noexcept {
        return tab().type == render::CreativeTabType::Search;
    }

    [[nodiscard]] const std::string& query() const noexcept { return search_.value(); }

    /// The search box itself — the same widget as the chat's, so Home/End,
    /// arrows and Ctrl/Cmd chords can reach it.
    [[nodiscard]] TextField& search_field() noexcept { return search_; }

    void type(std::string_view utf8);
    void backspace();

    /// Item tags, for a query that starts with '#': tag id → the items it
    /// holds, nested tags already resolved. Without it such a query matches
    /// nothing, and that is logged once.
    void set_item_tags(const std::unordered_map<std::string, std::unordered_set<std::string>>* tags) {
        tags_ = tags;
    }

    /// Whether a cell matches a query, by the rule measured on the running
    /// client (docs/provenance/inventaire-creatif.md § 9). Public for the
    /// parity check.
    [[nodiscard]] bool matches(const CreativeCell& cell, std::string_view folded_query) const;

    // ── Geometry ────────────────────────────────────────────────────────────

    /// The panel's corner: vanilla's integer (width − 195) / 2.
    [[nodiscard]] f32 origin_x(f32 screen_width) const noexcept;
    [[nodiscard]] f32 origin_y(f32 screen_height) const noexcept;

    /// A tab button's drawn top-left on the screen.
    [[nodiscard]] GuiPoint tab_origin(f32 screen_width, f32 screen_height,
                                      const render::CreativeTab& tab) const noexcept;

    [[nodiscard]] CreativeTarget hit_test(f32 screen_width, f32 screen_height, f32 mouse_x,
                                          f32 mouse_y) const noexcept;

    /// Where a window-0 slot is drawn on the current page, relative to the
    /// panel, or nothing when the page does not show it.
    [[nodiscard]] std::optional<GuiPoint> slot_position(i32 slot) const noexcept;

    // ── Drawing ─────────────────────────────────────────────────────────────

    [[nodiscard]] std::string_view background_texture() const noexcept;

    /// `player` is window 0 in slot order. `cursor_on` is the search field's
    /// blink phase.
    void draw(Gui& gui, const ItemRenderer& items, const CreativeTextures& textures,
              std::span<const ItemStackView> player, const CreativeTarget& hovered,
              bool cursor_on) const;

    /// The tooltip under the pointer, one `§`-coded line each; empty for none.
    /// `player_info` looks a player-slot stack up by item for its tooltip.
    void tooltip(const CreativeTarget& target, std::span<const ItemStackView> player,
                 std::vector<std::string>& out) const;

    /// The view a cell is drawn with: its tints and durability included.
    [[nodiscard]] static ItemStackView view_of(const CreativeCell& cell) noexcept;

private:
    void rebuild_page();
    void build_tab_cells();

    const render::CreativeTabs*  catalogue_{nullptr};
    const render::Language*      language_{nullptr};
    const render::CreativeItems* items_{nullptr};
    CreativeScreenOptions        options_;

    std::vector<const render::CreativeTab*> visible_;
    /// The cells of each visible tab, built once.
    std::vector<std::vector<CreativeCell>> tab_cells_;
    /// Folded names, parallel to tab_cells_, for cells without a tooltip.
    std::vector<std::vector<std::string>> fallback_names_;

    usize       selected_{0};
    f32         scroll_{0.0F};
    /// The search box: vanilla's EditBox, as the chat's is (text_field.hpp).
    TextField   search_{creative_layout::kSearchMaxLength};
    std::string folded_;

    std::vector<CreativeCell> page_;
    /// Indices into the selected tab's cells, parallel to page_, so the
    /// fallback name of a page cell can be found.
    std::vector<usize> page_source_;

    const SavedHotbars*      hotbars_{nullptr};
    std::vector<std::string> hint_lines_;
    std::vector<CreativeCell> hotbar_cells_;

    const std::unordered_map<std::string, std::unordered_set<std::string>>* tags_{nullptr};
};

/// The `Damage` int at the top level of a Slot's NBT (TAG_Compound, empty
/// name, payload), or 0. What the durability bar reads.
[[nodiscard]] i32 nbt_damage(std::span<const u8> nbt) noexcept;

/// Vanilla's tooltip: the dark box with the purple frame, the first line two
/// pixels further from the rest, twelve right of and twelve above the
/// pointer, pushed back on screen when it would leave it.
void draw_tooltip(Gui& gui, std::span<const std::string> lines, f32 mouse_x, f32 mouse_y);

}  // namespace ov::client
