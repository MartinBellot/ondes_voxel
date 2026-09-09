#define OV_LOG_CATEGORY "client"

#include "ov/client/hud.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ov::client {

namespace {

// ── Sprite rects, in the sheets' own logical pixels ──────────────────────────
//
// Every one of these was found by scripts/measure_gui_sprites.py, which walks
// the sheet in nine-pixel cells and reports how many texels are opaque and what
// colour they average. That is how the gold absorbing hearts at (160, 0) were
// told from the olive poisoned ones at (88, 0) and the near-black withered ones
// at (124, 0) — three pairs that a layout written from memory gets wrong and
// that look almost right on screen.
//
// The sheets are addressed as 256×256 whatever their real resolution: Faithful
// 32x ships them at 512², and dividing by the logical size rather than the real
// one is what makes one layout serve every pack.

constexpr f32 kSheet = 256.0F;

// widgets.png
constexpr f32 kHotbarX = 0.0F, kHotbarY = 0.0F, kHotbarW = 182.0F, kHotbarH = 22.0F;
constexpr f32 kSelectionX = 0.0F, kSelectionY = 22.0F, kSelectionSize = 24.0F;
/// The off-hand frame, to the side of the hotbar.
constexpr f32 kOffHandX = 24.0F, kOffHandY = 22.0F, kOffHandW = 29.0F, kOffHandH = 24.0F;

// icons.png
constexpr f32 kCrosshairSize = 15.0F;
constexpr f32 kIcon          = 9.0F;
constexpr f32 kHeartEmpty = 16.0F, kHeartEmptyBlink = 25.0F;
constexpr f32 kHeartFull = 52.0F, kHeartHalf = 61.0F;
constexpr f32 kHeartAbsorbFull = 160.0F, kHeartAbsorbHalf = 169.0F;
constexpr f32 kHeartRow = 0.0F;
constexpr f32 kArmourRow = 9.0F;
constexpr f32 kArmourEmpty = 16.0F, kArmourHalf = 25.0F, kArmourFull = 34.0F;
constexpr f32 kBubbleRow = 18.0F;
constexpr f32 kBubbleFull = 16.0F, kBubblePopping = 25.0F;
constexpr f32 kFoodRow = 27.0F;
constexpr f32 kFoodEmpty = 16.0F, kFoodFull = 52.0F, kFoodHalf = 61.0F;
constexpr f32 kExperienceBarX = 0.0F, kExperienceBackY = 64.0F, kExperienceFrontY = 69.0F;
constexpr f32 kExperienceBarW = 182.0F, kExperienceBarH = 5.0F;

/// Vanilla's experience level colour.
constexpr u32 kExperienceGreen = 0xFF80FF20U;

}  // namespace

i32 armour_points(std::string_view item) noexcept {
    // Order: helmet, chestplate, leggings, boots.
    struct Piece {
        std::string_view name;
        i32             points;
    };
    static constexpr std::array<Piece, 25> kPieces{
        Piece{"minecraft:leather_helmet", 1},      Piece{"minecraft:leather_chestplate", 3},
        Piece{"minecraft:leather_leggings", 2},    Piece{"minecraft:leather_boots", 1},
        Piece{"minecraft:chainmail_helmet", 2},    Piece{"minecraft:chainmail_chestplate", 5},
        Piece{"minecraft:chainmail_leggings", 4},  Piece{"minecraft:chainmail_boots", 1},
        Piece{"minecraft:iron_helmet", 2},         Piece{"minecraft:iron_chestplate", 6},
        Piece{"minecraft:iron_leggings", 5},       Piece{"minecraft:iron_boots", 2},
        Piece{"minecraft:golden_helmet", 2},       Piece{"minecraft:golden_chestplate", 5},
        Piece{"minecraft:golden_leggings", 3},     Piece{"minecraft:golden_boots", 1},
        Piece{"minecraft:diamond_helmet", 3},      Piece{"minecraft:diamond_chestplate", 8},
        Piece{"minecraft:diamond_leggings", 6},    Piece{"minecraft:diamond_boots", 3},
        Piece{"minecraft:netherite_helmet", 3},    Piece{"minecraft:netherite_chestplate", 8},
        Piece{"minecraft:netherite_leggings", 6},  Piece{"minecraft:netherite_boots", 3},
        Piece{"minecraft:turtle_helmet", 2},
    };
    for (const Piece& piece : kPieces) {
        if (piece.name == item) {
            return piece.points;
        }
    }
    return 0;
}

i32 armour_points(std::span<const ItemStackView> armour_slots) noexcept {
    i32 total = 0;
    for (const ItemStackView& slot : armour_slots) {
        if (!slot.empty()) {
            total += armour_points(slot.item);
        }
    }
    return total;
}

void draw_crosshair(Gui& gui, const HudTextures& textures) {
    const f32 x = std::floor((gui.width() - kCrosshairSize) * 0.5F);
    const f32 y = std::floor((gui.height() - kCrosshairSize) * 0.5F);
    gui.blit(textures.icons, x, y, kCrosshairSize, kCrosshairSize, 0.0F, 0.0F, kCrosshairSize,
             kCrosshairSize, kSheet, kSheet, 0xC0FFFFFFU);
}

void draw_hud(Gui& gui, const ItemRenderer& items, const HudTextures& textures,
              const HudState& state) {
    const f32 width  = gui.width();
    const f32 height = gui.height();
    // Vanilla's hotbar is 182 wide and centred on a whole pixel: half of an odd
    // width would put the whole bar on a half pixel and blur every slot.
    const f32 left  = std::floor(width * 0.5F) - 91.0F;
    const f32 right = std::floor(width * 0.5F) + 91.0F;

    draw_crosshair(gui, textures);

    // ── The hotbar ──────────────────────────────────────────────────────────
    gui.blit(textures.widgets, left, height - kHotbarH, kHotbarW, kHotbarH, kHotbarX, kHotbarY,
             kHotbarW, kHotbarH, kSheet, kSheet);
    const auto selected = static_cast<f32>(std::clamp(state.selected, 0, 8));
    gui.blit(textures.widgets, left - 1.0F + selected * 20.0F, height - kHotbarH - 1.0F,
             kSelectionSize, kSelectionSize, kSelectionX, kSelectionY, kSelectionSize,
             kSelectionSize, kSheet, kSheet);

    if (!state.off_hand.empty()) {
        gui.blit(textures.widgets, left - kOffHandW, height - kOffHandH + 1.0F, kOffHandW,
                 kOffHandH, kOffHandX, kOffHandY, kOffHandW, kOffHandH, kSheet, kSheet);
    }

    // Icons first, counts after. One batch of item quads, then one of text,
    // instead of nine of each.
    for (usize i = 0; i < state.hotbar.size(); ++i) {
        items.draw(gui, left + 3.0F + static_cast<f32>(i) * 20.0F, height - 19.0F,
                   state.hotbar[i]);
    }
    if (!state.off_hand.empty()) {
        items.draw(gui, left - kOffHandW + 6.0F, height - 19.0F, state.off_hand);
    }
    for (usize i = 0; i < state.hotbar.size(); ++i) {
        items.draw_count(gui, left + 3.0F + static_cast<f32>(i) * 20.0F, height - 19.0F,
                         state.hotbar[i]);
    }
    if (!state.off_hand.empty()) {
        items.draw_count(gui, left - kOffHandW + 6.0F, height - 19.0F, state.off_hand);
    }

    // ── The status bars ─────────────────────────────────────────────────────
    //
    // Creative has none of them: no hearts, no haunches, no experience. That is
    // not a simplification, it is what the game does, and a HUD that shows ten
    // full hearts in creative is showing a number nothing is maintaining.
    if (!state.creative) {
        const f32 top = height - 39.0F;

        // Experience, under the hearts and over the hotbar.
        gui.blit(textures.icons, left, height - 29.0F, kExperienceBarW, kExperienceBarH,
                 kExperienceBarX, kExperienceBackY, kExperienceBarW, kExperienceBarH, kSheet,
                 kSheet);
        const f32 filled =
            std::floor(std::clamp(state.experience_bar, 0.0F, 1.0F) * (kExperienceBarW + 1.0F));
        if (filled > 0.0F) {
            gui.blit(textures.icons, left, height - 29.0F, filled, kExperienceBarH,
                     kExperienceBarX, kExperienceFrontY, filled, kExperienceBarH, kSheet, kSheet);
        }
        if (state.experience_level > 0) {
            // Outlined rather than shadowed: vanilla draws the number four
            // times in black, one pixel out in each direction, then once in
            // green. A drop shadow would be unreadable over the green bar.
            const std::string text = std::to_string(state.experience_level);
            const f32         x    = std::floor(width * 0.5F);
            const f32         y    = height - 35.0F;
            for (const auto& [dx, dy] : std::array<std::pair<f32, f32>, 4>{
                     std::pair{1.0F, 0.0F}, {-1.0F, 0.0F}, {0.0F, 1.0F}, {0.0F, -1.0F}}) {
                (void)gui.text_centred(x + dx, y + dy, text, 0xFF000000U, false);
            }
            (void)gui.text_centred(x, y, text, kExperienceGreen, false);
        }

        // Hearts. Ten to a row, extra rows stacked upward, which is what a
        // health-boosted player sees.
        const i32 heart_count = std::max(1, static_cast<i32>(std::ceil(state.max_health / 2.0F)));
        const i32 rows        = (heart_count + 9) / 10;
        const f32 row_height  = static_cast<f32>(std::max(10 - (rows - 2), 3));
        const bool blink      = state.damage_flash > 0.0F;
        for (i32 i = 0; i < heart_count; ++i) {
            const f32 hx = left + static_cast<f32>(i % 10) * 8.0F;
            const f32 hy = top - static_cast<f32>(i / 10) * row_height;
            gui.blit(textures.icons, hx, hy, kIcon, kIcon,
                     blink ? kHeartEmptyBlink : kHeartEmpty, kHeartRow, kIcon, kIcon, kSheet,
                     kSheet);
            const f32 level = static_cast<f32>(i) * 2.0F;
            if (state.health >= level + 2.0F) {
                gui.blit(textures.icons, hx, hy, kIcon, kIcon, kHeartFull, kHeartRow, kIcon,
                         kIcon, kSheet, kSheet);
            } else if (state.health > level) {
                gui.blit(textures.icons, hx, hy, kIcon, kIcon, kHeartHalf, kHeartRow, kIcon,
                         kIcon, kSheet, kSheet);
            }
        }

        // Absorption, on top of the hearts, in gold. Never fed — see HudState.
        const i32 absorb = static_cast<i32>(std::ceil(state.absorption / 2.0F));
        for (i32 i = 0; i < absorb && i < 10; ++i) {
            const f32 hx = left + static_cast<f32>(i % 10) * 8.0F;
            const f32 hy = top - static_cast<f32>(i / 10) * 10.0F;
            const f32 level = static_cast<f32>(i) * 2.0F;
            gui.blit(textures.icons, hx, hy, kIcon, kIcon,
                     state.absorption >= level + 2.0F ? kHeartAbsorbFull : kHeartAbsorbHalf,
                     kHeartRow, kIcon, kIcon, kSheet, kSheet);
        }

        // Armour, a row above the hearts.
        if (state.armour > 0) {
            const f32 ay = top - 10.0F * static_cast<f32>(rows);
            for (i32 i = 0; i < 10; ++i) {
                const f32 ax    = left + static_cast<f32>(i) * 8.0F;
                const i32 level = i * 2;
                const f32 sprite = state.armour >= level + 2 ? kArmourFull
                                   : state.armour == level + 1 ? kArmourHalf
                                                               : kArmourEmpty;
                gui.blit(textures.icons, ax, ay, kIcon, kIcon, sprite, kArmourRow, kIcon, kIcon,
                         kSheet, kSheet);
            }
        }

        // Haunches, right-aligned and filled from the right, which is why the
        // loop runs the other way round from the hearts'.
        for (i32 i = 0; i < 10; ++i) {
            const f32 fx    = right - static_cast<f32>(i) * 8.0F - kIcon;
            const i32 level = i * 2;
            gui.blit(textures.icons, fx, top, kIcon, kIcon, kFoodEmpty, kFoodRow, kIcon, kIcon,
                     kSheet, kSheet);
            if (state.food >= level + 2) {
                gui.blit(textures.icons, fx, top, kIcon, kIcon, kFoodFull, kFoodRow, kIcon, kIcon,
                         kSheet, kSheet);
            } else if (state.food == level + 1) {
                gui.blit(textures.icons, fx, top, kIcon, kIcon, kFoodHalf, kFoodRow, kIcon, kIcon,
                         kSheet, kSheet);
            }
        }

        // Bubbles, only under water. Never fed — see HudState::air.
        if (state.air < state.max_air) {
            const f32 by = top - 10.0F;
            const i32 whole =
                static_cast<i32>(std::ceil(static_cast<f32>(state.air - 2) * 10.0F /
                                           static_cast<f32>(state.max_air)));
            const i32 popping =
                static_cast<i32>(std::ceil(static_cast<f32>(state.air) * 10.0F /
                                           static_cast<f32>(state.max_air))) -
                whole;
            for (i32 i = 0; i < whole + popping; ++i) {
                const f32 bx = right - static_cast<f32>(i) * 8.0F - kIcon;
                gui.blit(textures.icons, bx, by, kIcon, kIcon,
                         i < whole ? kBubbleFull : kBubblePopping, kBubbleRow, kIcon, kIcon,
                         kSheet, kSheet);
            }
        }
    }

    // ── The held item's name ────────────────────────────────────────────────
    if (state.name_flash_life > 0.0F && !state.name_flash.empty()) {
        // Vanilla holds the name for forty ticks and fades the last ten, so the
        // alpha is the life clamped to the last quarter and scaled back up.
        const f32 alpha = std::clamp(state.name_flash_life * 4.0F, 0.0F, 1.0F);
        const auto byte = static_cast<u32>(std::lround(alpha * 255.0F));
        if (byte > 0) {
            const f32 y = height - 59.0F + (state.creative ? 14.0F : 0.0F);
            (void)gui.text_centred(std::floor(width * 0.5F), y, state.name_flash,
                                   (byte << 24) | 0xFFFFFFU, true);
        }
    }
}

}  // namespace ov::client
