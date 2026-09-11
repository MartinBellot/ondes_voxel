#define OV_LOG_CATEGORY "client"

#include "ov/client/creative_screen.hpp"

#include "ov/base/log.hpp"
#include "ov/render/text_component.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {

using namespace creative_layout;

namespace {

constexpr f32 kSheet = 256.0F;

constexpr std::string_view kOperatorTab = "minecraft:op_blocks";

/// ASCII case folding. Vanilla folds with the root locale; for the names this
/// client ships, ASCII is all that changes.
void fold_into(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
}

[[nodiscard]] std::string key_of(std::string_view item, std::span<const u8> nbt) {
    std::string key(item);
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(nbt.data()), nbt.size());
    return key;
}

/// The "Damage" int of a Slot's NBT, without a full parse: the compound's
/// first level is scanned for a TAG_Int named Damage. Enough for the
/// durability bar; anything unexpected reads as undamaged.
[[nodiscard]] i32 damage_of(std::span<const u8> nbt) noexcept {
    // TAG_Compound (10), name length (2 bytes, 0), then entries.
    constexpr std::string_view kName = "Damage";
    if (nbt.size() < 3 || nbt[0] != 10) {
        return 0;
    }
    for (usize i = 3; i + 3 + kName.size() + 4 <= nbt.size(); ++i) {
        if (nbt[i] != 3 || nbt[i + 1] != 0 || nbt[i + 2] != kName.size()) {
            continue;
        }
        if (std::string_view(reinterpret_cast<const char*>(&nbt[i + 3]), kName.size()) != kName) {
            continue;
        }
        const usize at = i + 3 + kName.size();
        return static_cast<i32>((static_cast<u32>(nbt[at]) << 24) | (static_cast<u32>(nbt[at + 1]) << 16) |
                                (static_cast<u32>(nbt[at + 2]) << 8) | static_cast<u32>(nbt[at + 3]));
    }
    return 0;
}

}  // namespace

CreativeScreen::CreativeScreen(const render::CreativeTabs& tabs, const render::Language& language,
                               const render::CreativeItems* items, CreativeScreenOptions options)
    : catalogue_(&tabs), language_(&language), items_(items), options_(options) {
    for (const render::CreativeTab& tab : tabs.tabs()) {
        if (tab.id == kOperatorTab && !options_.operator_tab) {
            continue;
        }
        visible_.push_back(&tab);
    }
    build_tab_cells();
    // The tab vanilla opens on the first time: Building Blocks.
    if (!select("minecraft:building_blocks") && !visible_.empty()) {
        select(usize{0});
    }
}

void CreativeScreen::set_operator_tab(bool shown) {  // ── allow-commands ──
    if (shown == options_.operator_tab) {
        return;
    }
    const std::string current =
        visible_.empty() ? std::string{} : std::string{visible_[selected_]->id};
    options_.operator_tab     = shown;
    visible_.clear();
    for (const render::CreativeTab& tab : catalogue_->tabs()) {
        if (tab.id == kOperatorTab && !options_.operator_tab) {
            continue;
        }
        visible_.push_back(&tab);
    }
    build_tab_cells();
    selected_ = 0;
    if (!select(current) && !select("minecraft:building_blocks") && !visible_.empty()) {
        select(usize{0});
    }
}

void CreativeScreen::build_tab_cells() {
    // The stacks only the operator tab holds are absent everywhere when the
    // tab is: the running client's search page lacks them.
    std::unordered_set<std::string> operator_only;
    if (!options_.operator_tab) {
        if (const render::CreativeTab* op = catalogue_->find(kOperatorTab)) {
            for (const render::CreativeStack& stack : op->stacks) {
                operator_only.insert(key_of(stack.item, stack.nbt));
            }
        }
    }

    // The tooltips are keyed by occurrence in the search tab's order, so the
    // search tab is walked first and every other tab borrows its answers.
    std::unordered_map<std::string, const render::CreativeItemInfo*> info_of;
    if (items_ != nullptr) {
        if (const render::CreativeTab* search = catalogue_->find("minecraft:search")) {
            std::unordered_map<std::string_view, u32> seen;
            for (const render::CreativeStack& stack : search->stacks) {
                std::string key = key_of(stack.item, stack.nbt);
                if (operator_only.contains(key)) {
                    continue;
                }
                const u32 occurrence = seen[stack.item]++;
                info_of.emplace(std::move(key), items_->find(stack.item, occurrence));
            }
        }
    }

    tab_cells_.assign(visible_.size(), {});
    fallback_names_.assign(visible_.size(), {});
    std::string folded;
    for (usize t = 0; t < visible_.size(); ++t) {
        const render::CreativeTab& tab = *visible_[t];
        tab_cells_[t].reserve(tab.stacks.size());
        for (const render::CreativeStack& stack : tab.stacks) {
            std::string key = key_of(stack.item, stack.nbt);
            if (tab.type == render::CreativeTabType::Search && operator_only.contains(key)) {
                continue;
            }
            CreativeCell cell;
            cell.item  = stack.item;
            cell.count = stack.count;
            cell.nbt   = stack.nbt;
            if (const auto found = info_of.find(key); found != info_of.end()) {
                cell.info = found->second;
            }
            tab_cells_[t].push_back(cell);
            fold_into(language_->item_name(stack.item), folded);
            fallback_names_[t].push_back(folded);
        }
    }
}

void CreativeScreen::select(usize index) {
    if (index >= visible_.size()) {
        return;
    }
    selected_ = index;
    scroll_   = 0.0F;
    rebuild_page();
}

bool CreativeScreen::select(std::string_view id) {
    for (usize i = 0; i < visible_.size(); ++i) {
        if (visible_[i]->id == id) {
            select(i);
            return true;
        }
    }
    return false;
}

bool CreativeScreen::matches(const CreativeCell& cell, std::string_view query) const {
    if (query.empty()) {
        return true;
    }
    if (query.front() == '#') {
        // Tags: the tag's namespace and path each contain the given parts.
        if (tags_ == nullptr) {
            return false;
        }
        const std::string_view wanted = query.substr(1);
        const usize            colon  = wanted.find(':');
        for (const auto& [tag, members] : *tags_) {
            const usize            tag_colon = tag.find(':');
            const std::string_view ns   = std::string_view(tag).substr(0, tag_colon);
            const std::string_view path = std::string_view(tag).substr(tag_colon + 1);
            const bool hit = colon == std::string_view::npos
                                 ? path.find(wanted) != std::string_view::npos
                                 : ns.find(wanted.substr(0, colon)) != std::string_view::npos &&
                                       path.find(wanted.substr(colon + 1)) != std::string_view::npos;
            if (hit && members.contains(std::string(cell.item))) {
                return true;
            }
        }
        return false;
    }
    if (const usize colon = query.find(':'); colon != std::string_view::npos) {
        // An id: namespace and path each contain the given parts. Measured
        // only as far as "minecraft:" matching every stack.
        const usize            item_colon = cell.item.find(':');
        const std::string_view ns         = cell.item.substr(0, item_colon);
        const std::string_view path       = cell.item.substr(item_colon + 1);
        return ns.find(query.substr(0, colon)) != std::string_view::npos &&
               path.find(query.substr(colon + 1)) != std::string_view::npos;
    }
    // Text: a substring of the tooltip's text — every line, the name first —
    // without trimming. Measured: 16 of 17 queries give the running client's
    // exact list, in order.
    if (cell.info != nullptr) {
        return cell.info->search_text.find(query) != std::string::npos;
    }
    std::string folded;
    fold_into(language_->item_name(cell.item), folded);
    return folded.find(query) != std::string::npos;
}

void CreativeScreen::rebuild_page() {
    page_.clear();
    page_source_.clear();
    const render::CreativeTab& current = tab();
    if (current.type == render::CreativeTabType::Hotbar) {
        page_ = hotbar_cells_;
        scroll_ = std::clamp(scroll_, 0.0F, 1.0F);
        return;
    }
    if (current.type == render::CreativeTabType::Inventory) {
        return;
    }
    const std::vector<CreativeCell>& cells = tab_cells_[selected_];
    page_.reserve(cells.size());
    const bool filtered = searching() && !search_.value().empty();
    fold_into(search_.value(), folded_);
    for (usize i = 0; i < cells.size(); ++i) {
        if (filtered) {
            const bool hit = cells[i].info != nullptr
                                 ? matches(cells[i], folded_)
                                 : (folded_.find(':') == std::string::npos && folded_.front() != '#'
                                        ? fallback_names_[selected_][i].find(folded_) != std::string::npos
                                        : matches(cells[i], folded_));
            if (!hit) {
                continue;
            }
        }
        page_.push_back(cells[i]);
        page_source_.push_back(i);
    }
    scroll_ = std::clamp(scroll_, 0.0F, 1.0F);
}

void CreativeScreen::set_saved_hotbars(const SavedHotbars* hotbars, std::string_view save_key,
                                       const std::array<std::string, 9>& hotbar_keys) {
    hotbars_ = hotbars;
    hint_lines_.clear();
    // The running client's hint: a paper named with inventory.hotbarInfo, the
    // save key and the row's number key as arguments — a custom name, so
    // italic, and white.
    const auto escape = [](std::string_view text) {
        std::string out;
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out;
    };
    for (usize row = 0; row < hotbar_keys.size(); ++row) {
        const std::string json = std::string(R"({"italic":true,"color":"white","translate":)")
                               + R"("inventory.hotbarInfo","with":[{"text":")" + escape(save_key)
                               + R"("},{"text":")" + escape(hotbar_keys[row]) + R"("}]})";
        hint_lines_.push_back(render::flatten_component(json, *language_));
    }
    refresh_saved_hotbars();
}

void CreativeScreen::refresh_saved_hotbars() {
    hotbar_cells_.clear();
    hotbar_cells_.reserve(SavedHotbars::kRows * SavedHotbars::kColumns);
    for (usize row = 0; row < SavedHotbars::kRows; ++row) {
        const bool empty = hotbars_ == nullptr || hotbars_->row_empty(row);
        for (usize column = 0; column < SavedHotbars::kColumns; ++column) {
            CreativeCell cell;
            if (empty) {
                // Measured: an empty row shows its hint on the diagonal.
                if (column == row) {
                    cell.item      = "minecraft:paper";
                    cell.hint      = true;
                    cell.hint_line = row < hint_lines_.size() ? std::string_view(hint_lines_[row])
                                                              : std::string_view{};
                }
            } else {
                const SavedStack& saved = hotbars_->row(row)[column];
                if (!saved.empty()) {
                    cell.item  = saved.item;
                    cell.count = saved.count;
                    cell.nbt   = saved.nbt;
                    cell.info  = items_ != nullptr ? items_->first(saved.item) : nullptr;
                }
            }
            hotbar_cells_.push_back(cell);
        }
    }
    if (!visible_.empty() && tab().type == render::CreativeTabType::Hotbar) {
        rebuild_page();
    }
}

void CreativeScreen::type(std::string_view utf8) {
    if (!searching() || utf8.empty()) {
        return;
    }
    // The box holds 50 characters, and what may be typed into it is what may
    // be typed into chat: the same widget in vanilla, the same one here.
    if (!search_.insert(utf8)) {
        return;
    }
    scroll_ = 0.0F;
    rebuild_page();
}

void CreativeScreen::backspace() {
    if (!searching() || !search_.erase(-1, false)) {
        return;
    }
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
    return static_cast<i32>(scroll_ * static_cast<f32>(range) + 0.5F);
}

void CreativeScreen::scroll_by(f32 notches) {
    const i32 range = scroll_range();
    if (range <= 0) {
        return;
    }
    // Measured: one notch on Building Blocks moves scrollOffs by 1/34, one
    // over the rows beyond the first five.
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
    const f32 x = origin_x(screen_width) + tab_x(entry.column, entry.aligned_right);
    const f32 y = origin_y(screen_height) +
                  (entry.row == render::CreativeTabRow::Top ? kTopTabDrawY : kBottomTabDrawY);
    return GuiPoint{x, y};
}

const CreativeCell* CreativeScreen::cell(i32 index) const noexcept {
    if (index < 0 || index >= kPageCells) {
        return nullptr;
    }
    const auto absolute = static_cast<usize>(scroll_row() * kColumns + index);
    if (absolute >= page_.size() || page_[absolute].item.empty()) {
        return nullptr;
    }
    return &page_[absolute];
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

std::optional<GuiPoint> CreativeScreen::slot_position(i32 slot) const noexcept {
    if (slot >= 36 && slot <= 44) {
        return GuiPoint{kCellX + static_cast<f32>(slot - 36) * kCellPitch, kHotbarY};
    }
    if (tab().type != render::CreativeTabType::Inventory) {
        return std::nullopt;
    }
    if (slot >= 9 && slot <= 35) {
        const i32 index = slot - 9;
        return GuiPoint{kCellX + static_cast<f32>(index % kColumns) * kCellPitch,
                        kInventoryY + static_cast<f32>(index / kColumns) * kCellPitch};
    }
    if (slot >= 5 && slot <= 8) {
        const auto& at = kArmour[static_cast<usize>(slot - 5)];
        return GuiPoint{at[0], at[1]};
    }
    if (slot == 45) {
        return GuiPoint{kOffHand[0], kOffHand[1]};
    }
    return std::nullopt;
}

CreativeTarget CreativeScreen::hit_test(f32 screen_width, f32 screen_height, f32 mouse_x,
                                        f32 mouse_y) const noexcept {
    const f32 ox = origin_x(screen_width);
    const f32 oy = origin_y(screen_height);

    const auto inside = [&](f32 x, f32 y, f32 w, f32 h) {
        return mouse_x >= ox + x && mouse_x < ox + x + w && mouse_y >= oy + y && mouse_y < oy + y + h;
    };

    // Tabs first, on the running client's own rectangles: 26×32 at getTabX,
    // getTabY — above the panel's edge, not over the four pixels a button
    // overlaps it by.
    for (usize i = 0; i < visible_.size(); ++i) {
        const render::CreativeTab& entry = *visible_[i];
        const f32 y = entry.row == render::CreativeTabRow::Top ? kTopTabHitY : kBottomTabHitY;
        if (inside(tab_x(entry.column, entry.aligned_right), y, kTabWidth, kTabHeight)) {
            return CreativeTarget{CreativeHit::Tab, static_cast<i32>(i)};
        }
    }

    if (tab().type == render::CreativeTabType::Inventory) {
        if (inside(kDestroyX, kDestroyY, kCellSize, kCellSize)) {
            return CreativeTarget{CreativeHit::Destroy, -1};
        }
        for (i32 slot = 5; slot <= 45; ++slot) {
            const auto at = slot_position(slot);
            if (at && inside(at->x, at->y, kCellSize, kCellSize)) {
                return CreativeTarget{CreativeHit::PlayerSlot, slot};
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
            return CreativeTarget{CreativeHit::PlayerSlot, 36 + col};
        }
    }
    return CreativeTarget{};
}

ItemStackView CreativeScreen::view_of(const CreativeCell& cell) noexcept {
    ItemStackView view{cell.item, cell.count};
    if (cell.info != nullptr) {
        view.tints      = cell.info->tints;
        view.has_tints  = true;
        view.max_damage = cell.info->max_damage;
        view.damage     = damage_of(cell.nbt);
    }
    return view;
}

void CreativeScreen::draw(Gui& gui, const ItemRenderer& items, const CreativeTextures& textures,
                          std::span<const ItemStackView> player, const CreativeTarget& hovered,
                          bool cursor_on) const {
    const f32 ox = origin_x(gui.width());
    const f32 oy = origin_y(gui.height());

    // Buttons, then the panel, then the selected button over it; icons all in
    // one batch afterwards (one texture change instead of twenty-six).
    const auto blit_tab = [&](usize index, bool selected) {
        const render::CreativeTab& entry  = *visible_[index];
        const GuiPoint             origin = tab_origin(gui.width(), gui.height(), entry);
        const f32 band = (entry.row == render::CreativeTabRow::Top ? 0.0F : 64.0F)
                       + (selected ? kTabHeight : 0.0F);
        gui.blit(textures.tabs, origin.x, origin.y, kTabWidth, kTabHeight,
                 static_cast<f32>(entry.column) * kTabSheetPitch, band, kTabWidth, kTabHeight, kSheet,
                 kSheet);
    };
    for (usize i = 0; i < visible_.size(); ++i) {
        if (i != selected_) {
            blit_tab(i, false);
        }
    }
    gui.blit(textures.background, ox, oy, kPanelWidth, kPanelHeight, 0.0F, 0.0F, kPanelWidth,
             kPanelHeight, kSheet, kSheet);
    blit_tab(selected_, true);

    for (const render::CreativeTab* entry : visible_) {
        const GuiPoint origin = tab_origin(gui.width(), gui.height(), *entry);
        const f32      icon_y = origin.y + (entry->row == render::CreativeTabRow::Top
                                                ? kTopTabIconY
                                                : kBottomTabIconY);
        ItemStackView icon{entry->icon, 1};
        if (items_ != nullptr) {
            if (const render::CreativeItemInfo* info = items_->first(entry->icon)) {
                icon.tints     = info->tints;
                icon.has_tints = true;
            }
        }
        items.draw(gui, origin.x + kTabIconX, icon_y, icon);
    }

    // The title: every page but the survival one, including the search page
    // ("Search Items", left of the box) — measured on the running client.
    if (tab().type != render::CreativeTabType::Inventory) {
        (void)gui.text(ox + kTitleX, oy + kTitleY, language_->translate(tab().translation_key),
                       0xFF404040U, false);
    }

    if (searching()) {
        // No border: the text sits at the box's own corner, white, and the
        // cursor at the end is an underscore that blinks.
        const f32 pen = gui.text(ox + kSearchX, oy + kSearchY, search_.value(), 0xFFFFFFFFU, true);
        if (cursor_on) {
            (void)gui.text(pen, oy + kSearchY, "_", 0xFFFFFFFFU, true);
        }
    }

    if (tab().type == render::CreativeTabType::Inventory) {
        // Empty armour and off-hand slots show their silhouettes.
        const std::array<i32, 5> icon_slots{5, 6, 7, 8, 45};
        for (usize i = 0; i < icon_slots.size(); ++i) {
            const auto slot = static_cast<usize>(icon_slots[i]);
            const bool empty = slot >= player.size() || player[slot].empty();
            if (empty && textures.slot_icons[i]) {
                const auto at = slot_position(icon_slots[i]);
                items.draw_sprite(gui, ox + at->x, oy + at->y, *textures.slot_icons[i]);
            }
        }
    }

    // The cells: icons, then counts and bars.
    const bool shows_cells = tab().type != render::CreativeTabType::Inventory;
    if (shows_cells) {
        for (i32 i = 0; i < kPageCells; ++i) {
            if (const CreativeCell* stack = cell(i)) {
                items.draw(gui, ox + kCellX + static_cast<f32>(i % kColumns) * kCellPitch,
                           oy + kCellY + static_cast<f32>(i / kColumns) * kCellPitch, view_of(*stack));
            }
        }
    }
    for (i32 slot = 5; slot <= 45; ++slot) {
        const auto at = slot_position(slot);
        if (at && static_cast<usize>(slot) < player.size()) {
            items.draw(gui, ox + at->x, oy + at->y, player[static_cast<usize>(slot)]);
        }
    }
    if (shows_cells) {
        for (i32 i = 0; i < kPageCells; ++i) {
            if (const CreativeCell* stack = cell(i)) {
                items.draw_count(gui, ox + kCellX + static_cast<f32>(i % kColumns) * kCellPitch,
                                 oy + kCellY + static_cast<f32>(i / kColumns) * kCellPitch,
                                 view_of(*stack));
            }
        }
    }
    for (i32 slot = 5; slot <= 45; ++slot) {
        const auto at = slot_position(slot);
        if (at && static_cast<usize>(slot) < player.size()) {
            items.draw_count(gui, ox + at->x, oy + at->y, player[static_cast<usize>(slot)]);
        }
    }

    if (shows_cells && tab().type != render::CreativeTabType::Hotbar) {
        const f32 handle_y = oy + kScrollY
                           + (scrollable() ? scroll_ * (kScrollHeight - kHandleHeight) : 0.0F);
        gui.blit(textures.tabs, ox + kScrollX, handle_y, kScrollWidth, kHandleHeight,
                 scrollable() ? 232.0F : 244.0F, 0.0F, kScrollWidth, kHandleHeight, kSheet, kSheet);
    } else if (tab().type == render::CreativeTabType::Hotbar) {
        const f32 handle_y = oy + kScrollY + scroll_ * (kScrollHeight - kHandleHeight);
        gui.blit(textures.tabs, ox + kScrollX, handle_y, kScrollWidth, kHandleHeight, 232.0F, 0.0F,
                 kScrollWidth, kHandleHeight, kSheet, kSheet);
    }

    // The hover: white at half over the cell, on top of the item.
    if (hovered.kind == CreativeHit::Cell && cell(hovered.index) != nullptr) {
        gui.fill(ox + kCellX + static_cast<f32>(hovered.index % kColumns) * kCellPitch,
                 oy + kCellY + static_cast<f32>(hovered.index / kColumns) * kCellPitch, kCellSize,
                 kCellSize, 0x80FFFFFFU);
    } else if (hovered.kind == CreativeHit::PlayerSlot) {
        if (const auto at = slot_position(hovered.index)) {
            gui.fill(ox + at->x, oy + at->y, kCellSize, kCellSize, 0x80FFFFFFU);
        }
    } else if (hovered.kind == CreativeHit::Destroy) {
        gui.fill(ox + kDestroyX, oy + kDestroyY, kCellSize, kCellSize, 0x80FFFFFFU);
    }
}

void CreativeScreen::tooltip(const CreativeTarget& target, std::span<const ItemStackView> player,
                             std::vector<std::string>& out) const {
    out.clear();
    switch (target.kind) {
        case CreativeHit::Cell: {
            const CreativeCell* stack = cell(target.index);
            if (stack == nullptr) {
                return;
            }
            if (stack->hint) {
                out.emplace_back(stack->hint_line);
                return;
            }
            if (stack->info != nullptr) {
                const auto& lines = searching() ? stack->info->search_tooltip : stack->info->tooltip;
                out.assign(lines.begin(), lines.end());
                return;
            }
            out.emplace_back(language_->item_name(stack->item));
            return;
        }
        case CreativeHit::PlayerSlot: {
            const auto index = static_cast<usize>(target.index);
            if (index >= player.size() || player[index].empty()) {
                return;
            }
            const render::CreativeItemInfo* info =
                items_ != nullptr ? items_->first(player[index].item) : nullptr;
            if (info != nullptr) {
                out.assign(info->tooltip.begin(), info->tooltip.end());
            } else {
                out.emplace_back(language_->item_name(player[index].item));
            }
            return;
        }
        case CreativeHit::Tab:
            out.emplace_back(language_->translate(
                visible_[static_cast<usize>(target.index)]->translation_key));
            return;
        case CreativeHit::Destroy:
            out.emplace_back(language_->translate("inventory.binSlot"));
            return;
        case CreativeHit::None:
        case CreativeHit::Scrollbar:
        case CreativeHit::SearchField:
            return;
    }
}

i32 nbt_damage(std::span<const u8> nbt) noexcept {
    return damage_of(nbt);
}

void draw_tooltip(Gui& gui, std::span<const std::string> lines, f32 mouse_x, f32 mouse_y) {
    if (lines.empty()) {
        return;
    }
    f32 width = 0.0F;
    for (const std::string& line : lines) {
        width = std::max(width, gui.font().width(line));
    }
    const f32 height = lines.size() == 1 ? 8.0F : 10.0F + static_cast<f32>(lines.size() - 1) * 10.0F;
    f32 x = mouse_x + 12.0F;
    f32 y = mouse_y - 12.0F;
    if (x + width > gui.width()) {
        x = std::max(x - 24.0F - width, 4.0F);
    }
    if (y + height + 3.0F > gui.height()) {
        y = gui.height() - height - 3.0F;
    }

    constexpr u32 kBackground = 0xF0100010U;
    constexpr u32 kBorderTop  = 0x505000FFU;
    constexpr u32 kBorderLow  = 0x5028007FU;
    gui.fill(x - 3.0F, y - 4.0F, width + 6.0F, 1.0F, kBackground);
    gui.fill(x - 3.0F, y + height + 3.0F, width + 6.0F, 1.0F, kBackground);
    gui.fill(x - 3.0F, y - 3.0F, width + 6.0F, height + 6.0F, kBackground);
    gui.fill(x - 4.0F, y - 3.0F, 1.0F, height + 6.0F, kBackground);
    gui.fill(x + width + 3.0F, y - 3.0F, 1.0F, height + 6.0F, kBackground);
    gui.gradient(x - 3.0F, y - 2.0F, 1.0F, height + 4.0F, kBorderTop, kBorderLow);
    gui.gradient(x + width + 2.0F, y - 2.0F, 1.0F, height + 4.0F, kBorderTop, kBorderLow);
    gui.fill(x - 3.0F, y - 3.0F, width + 6.0F, 1.0F, kBorderTop);
    gui.fill(x - 3.0F, y + height + 2.0F, width + 6.0F, 1.0F, kBorderLow);

    f32 pen_y = y;
    for (usize i = 0; i < lines.size(); ++i) {
        (void)gui.text(x, pen_y, lines[i], 0xFFFFFFFFU, true);
        pen_y += i == 0 ? 12.0F : 10.0F;
    }
}

}  // namespace ov::client
