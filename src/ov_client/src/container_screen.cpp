#define OV_LOG_CATEGORY "client"

#include "ov/client/container_screen.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {

namespace {

/// Container backgrounds are 176 wide and their sheets are 256 square.
constexpr f32 kSheet     = 256.0F;
constexpr f32 kWindowW   = 176.0F;
constexpr f32 kSlotSize  = 16.0F;
constexpr f32 kSlotPitch = 18.0F;

/// Every one of these is the top-left of a 16×16 run of the slot grey in the
/// pack's own texture, found by scripts/measure_gui_sprites.py. `inventory.png`
/// yields exactly 46 of them and window 0 has exactly 46 slots, which is the
/// check that the layout and the protocol agree.
constexpr f32 kInventoryResultX = 154.0F, kInventoryResultY = 28.0F;
constexpr f32 kInventoryCraftX = 98.0F, kInventoryCraftY = 18.0F;
constexpr f32 kArmourX = 8.0F, kArmourY = 8.0F;
constexpr f32 kOffHandX = 77.0F, kOffHandY = 62.0F;
constexpr f32 kMainX = 8.0F, kMainY = 84.0F;
constexpr f32 kHotbarY = 142.0F;

constexpr f32 kCraftingGridX = 30.0F, kCraftingGridY = 17.0F;
constexpr f32 kCraftingResultX = 124.0F, kCraftingResultY = 35.0F;

constexpr f32 kFurnaceInputX = 56.0F, kFurnaceInputY = 17.0F;
constexpr f32 kFurnaceFuelY = 53.0F;
constexpr f32 kFurnaceOutputX = 116.0F, kFurnaceOutputY = 35.0F;

void add(std::vector<SlotRect>& slots, f32 x, f32 y) {
    slots.push_back(SlotRect{static_cast<i16>(slots.size()), x, y});
}

/// The player's three inventory rows and hotbar, which every window ends with.
void add_player_section(std::vector<SlotRect>& slots, f32 main_y, f32 hotbar_y) {
    for (i32 row = 0; row < 3; ++row) {
        for (i32 col = 0; col < 9; ++col) {
            add(slots, kMainX + static_cast<f32>(col) * kSlotPitch,
                main_y + static_cast<f32>(row) * kSlotPitch);
        }
    }
    for (i32 col = 0; col < 9; ++col) {
        add(slots, kMainX + static_cast<f32>(col) * kSlotPitch, hotbar_y);
    }
}

}  // namespace

ContainerScreen ContainerScreen::player_inventory() {
    ContainerScreen screen;
    screen.kind_      = ScreenKind::PlayerInventory;
    screen.window_id_ = 0;
    screen.title_     = "Inventory";
    screen.width_     = kWindowW;
    screen.height_    = 166.0F;

    // The order is the protocol's, not the layout's: slot 0 is the crafting
    // result, 1..4 the 2×2 grid, 5..8 the armour, 9..35 the inventory, 36..44
    // the hotbar, 45 the off hand. Getting this order wrong swaps a helmet
    // with a crafting ingredient and the server does exactly what it is told.
    add(screen.slots_, kInventoryResultX, kInventoryResultY);
    for (i32 row = 0; row < 2; ++row) {
        for (i32 col = 0; col < 2; ++col) {
            add(screen.slots_, kInventoryCraftX + static_cast<f32>(col) * kSlotPitch,
                kInventoryCraftY + static_cast<f32>(row) * kSlotPitch);
        }
    }
    for (i32 row = 0; row < 4; ++row) {
        add(screen.slots_, kArmourX, kArmourY + static_cast<f32>(row) * kSlotPitch);
    }
    add_player_section(screen.slots_, kMainY, kHotbarY);
    add(screen.slots_, kOffHandX, kOffHandY);
    return screen;
}

std::optional<ContainerScreen> ContainerScreen::from_menu(i32 menu_type, u8 window_id,
                                                          std::string title) {
    ContainerScreen screen;
    screen.window_id_ = window_id;
    screen.title_     = std::move(title);
    screen.width_     = kWindowW;

    // Menu ids are indices into `minecraft:menu`, which the client hard-codes
    // and never receives. 0..5 are generic_9x1..generic_9x6; 11 is crafting;
    // 9, 13 and 21 are the blast furnace, the furnace and the smoker.
    if (menu_type >= 0 && menu_type <= 5) {
        screen.kind_   = ScreenKind::Chest;
        screen.rows_   = menu_type + 1;
        screen.height_ = 114.0F + static_cast<f32>(screen.rows_) * kSlotPitch;
        for (i32 row = 0; row < screen.rows_; ++row) {
            for (i32 col = 0; col < 9; ++col) {
                add(screen.slots_, kMainX + static_cast<f32>(col) * kSlotPitch,
                    18.0F + static_cast<f32>(row) * kSlotPitch);
            }
        }
        // Vanilla's own offset: the player's section is 103 + (rows − 4) × 18
        // down from the top, and the hotbar 161 + the same. It puts a three-row
        // chest's inventory at 85 and a six-row one's at 139, which is what the
        // textures' own slot boxes measure.
        const f32 shift = static_cast<f32>(screen.rows_ - 4) * kSlotPitch;
        add_player_section(screen.slots_, 103.0F + shift, 161.0F + shift);
        return screen;
    }

    if (menu_type == 11) {
        screen.kind_   = ScreenKind::CraftingTable;
        screen.height_ = 166.0F;
        add(screen.slots_, kCraftingResultX, kCraftingResultY);
        for (i32 row = 0; row < 3; ++row) {
            for (i32 col = 0; col < 3; ++col) {
                add(screen.slots_, kCraftingGridX + static_cast<f32>(col) * kSlotPitch,
                    kCraftingGridY + static_cast<f32>(row) * kSlotPitch);
            }
        }
        add_player_section(screen.slots_, kMainY, kHotbarY);
        return screen;
    }

    if (menu_type == 9 || menu_type == 13 || menu_type == 21) {
        screen.kind_   = ScreenKind::Furnace;
        screen.height_ = 166.0F;
        add(screen.slots_, kFurnaceInputX, kFurnaceInputY);
        add(screen.slots_, kFurnaceInputX, kFurnaceFuelY);
        add(screen.slots_, kFurnaceOutputX, kFurnaceOutputY);
        add_player_section(screen.slots_, kMainY, kHotbarY);
        return screen;
    }

    // Refused and named. Drawing an anvil as a chest would put its slots where
    // the player's inventory is, and every click would name a slot that means
    // something else on the server.
    OV_LOG_WARN("menu type {} is not implemented; the window is closed instead", menu_type);
    return std::nullopt;
}

std::string_view ContainerScreen::background_texture() const noexcept {
    switch (kind_) {
        case ScreenKind::PlayerInventory:
            return "minecraft:gui/container/inventory";
        case ScreenKind::Chest:
            return "minecraft:gui/container/generic_54";
        case ScreenKind::CraftingTable:
            return "minecraft:gui/container/crafting_table";
        case ScreenKind::Furnace:
            return "minecraft:gui/container/furnace";
    }
    return "minecraft:gui/container/generic_54";
}

f32 ContainerScreen::origin_x(f32 screen_width) const noexcept {
    return std::floor((screen_width - width_) * 0.5F);
}

f32 ContainerScreen::origin_y(f32 screen_height) const noexcept {
    return std::floor((screen_height - height_) * 0.5F);
}

const SlotRect* ContainerScreen::slot_at(f32 screen_width, f32 screen_height, f32 mouse_x,
                                         f32 mouse_y) const noexcept {
    const f32 ox = origin_x(screen_width);
    const f32 oy = origin_y(screen_height);
    for (const SlotRect& slot : slots_) {
        const f32 x = ox + slot.x;
        const f32 y = oy + slot.y;
        if (mouse_x >= x && mouse_x < x + kSlotSize && mouse_y >= y && mouse_y < y + kSlotSize) {
            return &slot;
        }
    }
    return nullptr;
}

ClickIntent ContainerScreen::click(i16 slot, i32 button, bool shift, i32 hotbar_key) noexcept {
    // A number key while the cursor is over a slot swaps that slot with the
    // hotbar slot the key names — mode 2, with the *hotbar index* as the
    // button. It is the one mode where the button is not a mouse button, and
    // reading it as one is how a number key becomes a right-click.
    if (hotbar_key >= 0 && hotbar_key < 9) {
        return ClickIntent{slot, static_cast<i8>(hotbar_key), click_mode::kSwap};
    }
    if (button == 2) {
        // Middle click clones in creative and does nothing otherwise. Sent
        // either way: the server decides, not us.
        return ClickIntent{slot, 2, click_mode::kClone};
    }
    if (shift) {
        return ClickIntent{slot, static_cast<i8>(button == 1 ? 1 : 0), click_mode::kQuickMove};
    }
    return ClickIntent{slot, static_cast<i8>(button == 1 ? 1 : 0), click_mode::kPickup};
}

void ContainerScreen::draw(Gui& gui, const ItemRenderer& items, GuiTexture background,
                           std::span<const ItemStackView> contents,
                           const SlotRect*                hovered) const {
    const f32 ox = origin_x(gui.width());
    const f32 oy = origin_y(gui.height());

    if (kind_ == ScreenKind::Chest) {
        // Two blits, because generic_54.png holds a six-row chest and a
        // three-row one is the top of it plus the bottom of it. The seam is at
        // texture row 126, which is where the player's section starts.
        const f32 top = static_cast<f32>(rows_) * kSlotPitch + 17.0F;
        gui.blit(background, ox, oy, width_, top, 0.0F, 0.0F, width_, top, kSheet, kSheet);
        gui.blit(background, ox, oy + top, width_, 96.0F, 0.0F, 126.0F, width_, 96.0F, kSheet,
                 kSheet);
    } else {
        gui.blit(background, ox, oy, width_, height_, 0.0F, 0.0F, width_, height_, kSheet,
                 kSheet);
    }

    // The title, where vanilla puts it: eight pixels in, six down, in the dark
    // grey the backgrounds are drawn for, and with no shadow.
    (void)gui.text(ox + 8.0F, oy + 6.0F, title_, 0xFF404040U, false);
    if (kind_ != ScreenKind::PlayerInventory) {
        // "Inventory", over the player's own section. Vanilla labels it in
        // every container screen and not in the inventory screen itself.
        (void)gui.text(ox + 8.0F, oy + height_ - 94.0F, "Inventory", 0xFF404040U, false);
    }

    // Icons, then counts. Two passes for the same reason the hotbar uses two:
    // it is one batch each instead of one per slot, and a count is never buried
    // under the next slot's quads.
    for (const SlotRect& slot : slots_) {
        const auto index = static_cast<usize>(slot.index);
        if (index >= contents.size()) {
            continue;
        }
        items.draw(gui, ox + slot.x, oy + slot.y, contents[index]);
    }
    for (const SlotRect& slot : slots_) {
        const auto index = static_cast<usize>(slot.index);
        if (index >= contents.size()) {
            continue;
        }
        items.draw_count(gui, ox + slot.x, oy + slot.y, contents[index]);
    }

    if (hovered != nullptr) {
        // Vanilla's hover: white at half over the cell, drawn last so it is on
        // top of the item rather than under it.
        gui.fill(ox + hovered->x, oy + hovered->y, kSlotSize, kSlotSize, 0x80FFFFFFU);
    }
}

void draw_screen_dim(Gui& gui) {
    gui.gradient(0.0F, 0.0F, gui.width(), gui.height(), 0x10101010U, 0xC0101010U);
}

}  // namespace ov::client
