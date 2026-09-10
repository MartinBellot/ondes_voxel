#define OV_LOG_CATEGORY "client"

#include "ov/client/creative_screen.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {

using namespace creative_layout;

namespace {

constexpr f32 kSheet = 256.0F;

/// The window-0 slot numbers of the player's hotbar, which a category page
/// shows along its bottom edge.
constexpr i32 kHotbarFirstSlot = 36;

/// ASCII case folding, which is all the search needs: the names it compares
/// are translations from `lang/en_us.json`, and vanilla's own creative search
/// lower-cases with the root locale.
void fold_into(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
}

}  // namespace

CreativeScreen::CreativeScreen(const render::CreativeTabs& tabs, const render::Language& language)
    : tabs_(&tabs), language_(&language) {
    // The tab vanilla opens on. Not the first in the registry: the game
    // remembers the last one, and starts on Building Blocks.
    if (!select("minecraft:building_blocks") && !tabs.tabs().empty()) {
        select(usize{0});
    }
}

void CreativeScreen::select(usize index) {
    if (index >= tabs_->tabs().size()) {
        return;
    }
    selected_ = index;
    scroll_   = 0.0F;

    // Translate once per tab, not once per keystroke. The search page holds
    // 1587 cells and a filter that translated each of them on every character
    // would do a million lookups to type "stone".
    const render::CreativeTab& current = tab();
    names_.clear();
    names_.reserve(current.stacks.size());
    std::string folded;
    for (const render::CreativeStack& stack : current.stacks) {
        fold_into(language_->item_name(stack.item), folded);
        names_.push_back(folded);
    }
    rebuild_page();
}

bool CreativeScreen::select(std::string_view id) {
    const auto& all = tabs_->tabs();
    for (usize i = 0; i < all.size(); ++i) {
        if (all[i].id == id) {
            select(i);
            return true;
        }
    }
    return false;
}

void CreativeScreen::rebuild_page() {
    const render::CreativeTab& current = tab();
    page_.clear();
    page_.reserve(current.stacks.size());
    const bool filtered = searching() && !query_.empty();
    fold_into(query_, folded_);
    for (usize i = 0; i < current.stacks.size(); ++i) {
        if (filtered && names_[i].find(folded_) == std::string::npos) {
            continue;
        }
        page_.push_back(&current.stacks[i]);
    }
    scroll_ = std::clamp(scroll_, 0.0F, 1.0F);
}

void CreativeScreen::type(std::string_view utf8) {
    if (!searching() || utf8.empty()) {
        return;
    }
    query_.append(utf8);
    scroll_ = 0.0F;
    rebuild_page();
}

void CreativeScreen::backspace() {
    if (!searching() || query_.empty()) {
        return;
    }
    // Back off a whole UTF-8 sequence, not a byte: deleting one byte of a
    // two-byte character leaves an invalid string the font would refuse.
    usize cut = query_.size() - 1;
    while (cut > 0 && (static_cast<u8>(query_[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    query_.resize(cut);
    scroll_ = 0.0F;
    rebuild_page();
}

i32 CreativeScreen::scroll_range() const noexcept {
    const auto rows = static_cast<i32>((page_.size() + kColumns - 1) / kColumns);
    return std::max(0, rows - kRows);
}

i32 CreativeScreen::scroll_row() const noexcept {
    const i32 range = scroll_range();
    if (range <= 0) {
        return 0;
    }
    // Vanilla rounds rather than truncating, so the top row changes when the
    // handle passes the middle of a step instead of at its very end.
    return static_cast<i32>(scroll_ * static_cast<f32>(range) + 0.5F);
}

void CreativeScreen::scroll_by(f32 notches) {
    const i32 range = scroll_range();
    if (range <= 0) {
        return;
    }
    scroll_ = std::clamp(scroll_ - notches / static_cast<f32>(range), 0.0F, 1.0F);
}

void CreativeScreen::drag_scroll(f32 screen_height, f32 mouse_y) {
    if (scroll_range() <= 0) {
        return;
    }
    const f32 top    = origin_y(screen_height) + kScrollY;
    const f32 travel = kScrollHeight - kHandleHeight;
    scroll_ = std::clamp((mouse_y - top - kHandleHeight * 0.5F) / travel, 0.0F, 1.0F);
}

f32 CreativeScreen::origin_x(f32 screen_width) const noexcept {
    return std::floor((screen_width - kPanelWidth) * 0.5F);
}

f32 CreativeScreen::origin_y(f32 screen_height) const noexcept {
    return std::floor((screen_height - kPanelHeight) * 0.5F);
}

GuiPoint CreativeScreen::tab_origin(f32 screen_width, f32 screen_height,
                                    const render::CreativeTab& entry) const noexcept {
    const f32 ox = origin_x(screen_width);
    const f32 oy = origin_y(screen_height);
    const f32 x  = ox + static_cast<f32>(entry.column) * kTabPitch;
    const f32 y  = entry.row == render::CreativeTabRow::Top
                     ? oy - kTabHeight + kTabOverlap
                     : oy + kPanelHeight - kTabOverlap;
    return GuiPoint{x, y};
}

const render::CreativeStack* CreativeScreen::cell(i32 index) const noexcept {
    if (index < 0 || index >= kPageCells) {
        return nullptr;
    }
    const auto absolute = static_cast<usize>(scroll_row() * kColumns + index);
    return absolute < page_.size() ? page_[absolute] : nullptr;
}

std::string_view CreativeScreen::background_texture() const noexcept {
    switch (tab().type) {
        case render::CreativeTabType::Search:
            return "minecraft:gui/container/creative_inventory/tab_item_search";
        case render::CreativeTabType::Inventory:
            return "minecraft:gui/container/creative_inventory/tab_inventory";
        case render::CreativeTabType::Category:
        case render::CreativeTabType::Hotbar:
            break;
    }
    return "minecraft:gui/container/creative_inventory/tab_items";
}

CreativeTarget CreativeScreen::hit_test(f32 screen_width, f32 screen_height, f32 mouse_x,
                                        f32 mouse_y) const noexcept {
    const f32 ox = origin_x(screen_width);
    const f32 oy = origin_y(screen_height);

    const auto inside = [&](f32 x, f32 y, f32 w, f32 h) {
        return mouse_x >= ox + x && mouse_x < ox + x + w && mouse_y >= oy + y
            && mouse_y < oy + y + h;
    };

    // Tabs first: they overhang the panel, and a click in the overlap belongs
    // to the button that is drawn on top.
    const auto& all = tabs_->tabs();
    for (usize i = 0; i < all.size(); ++i) {
        const GuiPoint origin = tab_origin(screen_width, screen_height, all[i]);
        if (mouse_x >= origin.x && mouse_x < origin.x + kTabWidth && mouse_y >= origin.y
            && mouse_y < origin.y + kTabHeight - kTabOverlap) {
            return CreativeTarget{CreativeHit::Tab, static_cast<i32>(i)};
        }
    }

    if (tab().type == render::CreativeTabType::Inventory) {
        if (inside(kDestroyX, kDestroyY, kCellSize, kCellSize)) {
            return CreativeTarget{CreativeHit::Destroy, -1};
        }
        // Three rows of nine at y = 54, 72, 90 are window-0 slots 9..35; the
        // hotbar at y = 112 is 36..44. Both were counted in the texture.
        for (i32 row = 0; row < 3; ++row) {
            for (i32 col = 0; col < kColumns; ++col) {
                if (inside(kCellX + static_cast<f32>(col) * kCellPitch,
                           54.0F + static_cast<f32>(row) * kCellPitch, kCellSize, kCellSize)) {
                    return CreativeTarget{CreativeHit::PlayerSlot, 9 + row * kColumns + col};
                }
            }
        }
        for (i32 col = 0; col < kColumns; ++col) {
            if (inside(kCellX + static_cast<f32>(col) * kCellPitch, kHotbarY, kCellSize,
                       kCellSize)) {
                return CreativeTarget{CreativeHit::PlayerSlot, kHotbarFirstSlot + col};
            }
        }
        return CreativeTarget{};
    }

    if (searching() && inside(kSearchX, kSearchY, kSearchWidth, kSearchHeight)) {
        return CreativeTarget{CreativeHit::SearchField, -1};
    }

    if (scrollable() && inside(kScrollX, kScrollY, kScrollWidth, kScrollHeight)) {
        return CreativeTarget{CreativeHit::Scrollbar, -1};
    }

    for (i32 row = 0; row < kRows; ++row) {
        for (i32 col = 0; col < kColumns; ++col) {
            if (inside(kCellX + static_cast<f32>(col) * kCellPitch,
                       kCellY + static_cast<f32>(row) * kCellPitch, kCellSize, kCellSize)) {
                return CreativeTarget{CreativeHit::Cell, row * kColumns + col};
            }
        }
    }

    for (i32 col = 0; col < kColumns; ++col) {
        if (inside(kCellX + static_cast<f32>(col) * kCellPitch, kHotbarY, kCellSize, kCellSize)) {
            return CreativeTarget{CreativeHit::PlayerSlot, kHotbarFirstSlot + col};
        }
    }
    return CreativeTarget{};
}

void CreativeScreen::draw(Gui& gui, const ItemRenderer& items, GuiTexture background,
                          GuiTexture tab_sheet, std::span<const ItemStackView> player,
                          const CreativeTarget& hovered) const {
    const f32 ox = origin_x(gui.width());
    const f32 oy = origin_y(gui.height());

    // Buttons first, icons afterwards — never one button then its icon.
    //
    // The batcher cuts a draw at every texture change, so alternating the tab
    // sheet with the block atlas thirteen times costs twenty-six draws. Drawn
    // in two passes it costs three: the unselected buttons, the selected one
    // (which has to come after the panel so it looks joined to the page), and
    // one batch for every icon. Measured: 37 draws before, 11 after, on the
    // same page.
    const auto& all      = tabs_->tabs();
    const auto  blit_tab = [&](usize index, bool selected) {
        const render::CreativeTab& entry  = all[index];
        const GuiPoint             origin = tab_origin(gui.width(), gui.height(), entry);
        const f32 band = (entry.row == render::CreativeTabRow::Top ? 0.0F : 64.0F)
                       + (selected ? kTabHeight : 0.0F);
        gui.blit(tab_sheet, origin.x, origin.y, kTabWidth, kTabHeight,
                 static_cast<f32>(entry.column) * kTabSheetPitch, band, kTabWidth, kTabHeight,
                 kSheet, kSheet);
    };

    for (usize i = 0; i < all.size(); ++i) {
        if (i != selected_) {
            blit_tab(i, false);
        }
    }

    gui.blit(background, ox, oy, kPanelWidth, kPanelHeight, 0.0F, 0.0F, kPanelWidth, kPanelHeight,
             kSheet, kSheet);
    blit_tab(selected_, true);

    // The icons, all of them, in one batch. They sit outside the panel, so
    // drawing them after it changes nothing but the draw count.
    for (const render::CreativeTab& entry : all) {
        const GuiPoint origin = tab_origin(gui.width(), gui.height(), entry);
        const f32      icon_x = origin.x + (kTabWidth - 16.0F) * 0.5F;
        const f32      icon_y = entry.row == render::CreativeTabRow::Top ? origin.y + 6.0F
                                                                        : origin.y + 9.0F;
        items.draw(gui, icon_x, icon_y, ItemStackView{entry.icon, 1});
    }

    // The tab's own name, where vanilla puts a container title — and only on
    // a page that has room for one. The search page's text field starts at
    // x = 82 and the survival page's armour boxes at y = 6, so a label there
    // would be drawn straight through them, which is what the first capture
    // showed.
    if (tab().type == render::CreativeTabType::Category
        || tab().type == render::CreativeTabType::Hotbar) {
        (void)gui.text(ox + 8.0F, oy + 6.0F, language_->translate(tab().translation_key),
                       0xFF404040U, false);
    }

    if (tab().type == render::CreativeTabType::Hotbar) {
        // Refused and named. The saved hotbars live in the vanilla client's
        // own options file, not in the game or on the wire; drawing nine empty
        // rows under that button would promise a feature that does not exist
        // here.
        (void)gui.text_centred(ox + kPanelWidth * 0.5F, oy + 60.0F,
                               "Saved hotbars are not implemented", 0xFF404040U, false);
        return;
    }

    if (tab().type == render::CreativeTabType::Inventory) {
        for (i32 slot = 9; slot < 45; ++slot) {
            const i32 index = slot < kHotbarFirstSlot ? slot - 9 : slot - kHotbarFirstSlot;
            const f32 x = ox + kCellX + static_cast<f32>(index % kColumns) * kCellPitch;
            const f32 y = slot < kHotbarFirstSlot
                            ? oy + 54.0F + static_cast<f32>(index / kColumns) * kCellPitch
                            : oy + kHotbarY;
            if (static_cast<usize>(slot) < player.size()) {
                items.draw(gui, x, y, player[static_cast<usize>(slot)]);
                items.draw_count(gui, x, y, player[static_cast<usize>(slot)]);
            }
        }
        if (hovered.kind == CreativeHit::PlayerSlot) {
            const i32 index = hovered.index < kHotbarFirstSlot ? hovered.index - 9
                                                               : hovered.index - kHotbarFirstSlot;
            const f32 x = ox + kCellX + static_cast<f32>(index % kColumns) * kCellPitch;
            const f32 y = hovered.index < kHotbarFirstSlot
                            ? oy + 54.0F + static_cast<f32>(index / kColumns) * kCellPitch
                            : oy + kHotbarY;
            gui.fill(x, y, kCellSize, kCellSize, 0x80FFFFFFU);
        } else if (hovered.kind == CreativeHit::Destroy) {
            gui.fill(ox + kDestroyX, oy + kDestroyY, kCellSize, kCellSize, 0x80FFFFFFU);
        }
        return;
    }

    if (searching()) {
        // The field's own text. Vanilla draws it white on the sunken box, with
        // a cursor that blinks; the cursor here is a steady bar, because a
        // blink needs a clock the interface does not have.
        const f32 pen = gui.text(ox + kSearchX + 4.0F, oy + kSearchY + 4.0F, query_, 0xFFFFFFFFU,
                                 false);
        gui.fill(pen, oy + kSearchY + 3.0F, 1.0F, 10.0F, 0xFFD0D0D0U);
    }

    // Icons, then counts, then the hover: one batch each rather than one per
    // cell, and a count is never buried under the next cell's quads.
    for (i32 i = 0; i < kPageCells; ++i) {
        const render::CreativeStack* stack = cell(i);
        if (stack == nullptr) {
            continue;
        }
        const f32 x = ox + kCellX + static_cast<f32>(i % kColumns) * kCellPitch;
        const f32 y = oy + kCellY + static_cast<f32>(i / kColumns) * kCellPitch;
        items.draw(gui, x, y, ItemStackView{stack->item, stack->count});
    }
    for (i32 i = 0; i < kPageCells; ++i) {
        const render::CreativeStack* stack = cell(i);
        if (stack == nullptr) {
            continue;
        }
        const f32 x = ox + kCellX + static_cast<f32>(i % kColumns) * kCellPitch;
        const f32 y = oy + kCellY + static_cast<f32>(i / kColumns) * kCellPitch;
        items.draw_count(gui, x, y, ItemStackView{stack->item, stack->count});
    }

    for (i32 col = 0; col < kColumns; ++col) {
        const auto slot = static_cast<usize>(kHotbarFirstSlot + col);
        if (slot >= player.size()) {
            break;
        }
        const f32 x = ox + kCellX + static_cast<f32>(col) * kCellPitch;
        items.draw(gui, x, oy + kHotbarY, player[slot]);
        items.draw_count(gui, x, oy + kHotbarY, player[slot]);
    }

    // The scrollbar handle: 12×15, at the top of a 110-tall groove, travelling
    // 95 pixels. The second sprite in the sheet is the greyed-out one, which is
    // what a page that fits shows.
    const f32 handle_y = oy + kScrollY
                       + (scrollable() ? scroll_ * (kScrollHeight - kHandleHeight) : 0.0F);
    gui.blit(tab_sheet, ox + kScrollX, handle_y, kScrollWidth, kHandleHeight,
             scrollable() ? 232.0F : 244.0F, 0.0F, kScrollWidth, kHandleHeight, kSheet, kSheet);

    if (hovered.kind == CreativeHit::Cell && cell(hovered.index) != nullptr) {
        gui.fill(ox + kCellX + static_cast<f32>(hovered.index % kColumns) * kCellPitch,
                 oy + kCellY + static_cast<f32>(hovered.index / kColumns) * kCellPitch, kCellSize,
                 kCellSize, 0x80FFFFFFU);
    } else if (hovered.kind == CreativeHit::PlayerSlot) {
        gui.fill(ox + kCellX
                     + static_cast<f32>(hovered.index - kHotbarFirstSlot) * kCellPitch,
                 oy + kHotbarY, kCellSize, kCellSize, 0x80FFFFFFU);
    }
}

std::string_view CreativeScreen::hovered_name(const CreativeTarget&          target,
                                              std::span<const ItemStackView> player) const {
    switch (target.kind) {
        case CreativeHit::Cell: {
            const render::CreativeStack* stack = cell(target.index);
            return stack == nullptr ? std::string_view{} : language_->item_name(stack->item);
        }
        case CreativeHit::PlayerSlot: {
            const auto index = static_cast<usize>(target.index);
            if (index >= player.size() || player[index].empty()) {
                return {};
            }
            return language_->item_name(player[index].item);
        }
        case CreativeHit::Tab:
            return language_->translate(tabs_->tabs()[static_cast<usize>(target.index)]
                                            .translation_key);
        case CreativeHit::Destroy:
            return "Destroy Item";
        case CreativeHit::None:
        case CreativeHit::Scrollbar:
        case CreativeHit::SearchField:
            break;
    }
    return {};
}

}  // namespace ov::client
