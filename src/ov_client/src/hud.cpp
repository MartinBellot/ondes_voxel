#define OV_LOG_CATEGORY "client"

#include "ov/client/hud.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace ov::client {

namespace {

// ── Sprite rects, in the sheets' own logical pixels ──────────────────────────
//
// Every one of these was found by scripts/measure_gui_sprites.py, which walks
// the sheet in nine-pixel cells and reports how many texels are opaque and what
// colour they average (docs/provenance/interface.md). The sheets are addressed
// as 256×256 whatever their real resolution.

constexpr f32 kSheet = 256.0F;

// widgets.png
constexpr f32 kHotbarX = 0.0F, kHotbarY = 0.0F, kHotbarW = 182.0F, kHotbarH = 22.0F;
constexpr f32 kSelectionX = 0.0F, kSelectionY = 22.0F, kSelectionSize = 24.0F;
/// The off-hand frame, to the side of the hotbar.
constexpr f32 kOffHandX = 24.0F, kOffHandY = 22.0F, kOffHandW = 29.0F, kOffHandH = 24.0F;

// icons.png
constexpr f32 kCrosshairSize = 15.0F;
constexpr f32 kIcon          = 9.0F;
constexpr f32 kHeartRow = 0.0F, kHardcoreHeartRow = 45.0F;
constexpr f32 kArmourRow = 9.0F;
constexpr f32 kArmourEmpty = 16.0F, kArmourHalf = 25.0F, kArmourFull = 34.0F;
/// A mount's hearts, on the armour row: container, full, half.
constexpr f32 kMountContainer = 52.0F, kMountFull = 88.0F, kMountHalf = 97.0F;
constexpr f32 kBubbleRow = 18.0F;
constexpr f32 kBubbleFull = 16.0F, kBubblePopping = 25.0F;
constexpr f32 kFoodRow = 27.0F;
constexpr f32 kFoodEmpty = 16.0F, kFoodFull = 52.0F, kFoodHalf = 61.0F;
/// Hunger: the container thirteen cells on, the haunch four.
constexpr f32 kHungerEmpty = 133.0F, kHungerShift = 36.0F;
constexpr f32 kBarX = 0.0F, kBarW = 182.0F, kBarH = 5.0F;
constexpr f32 kExperienceBackY = 64.0F, kExperienceFrontY = 69.0F;
constexpr f32 kJumpBackY = 84.0F, kJumpFrontY = 89.0F;

/// Vanilla's experience level colour.
constexpr u32 kExperienceGreen = 0xFF80FF20U;

/// Frozen hearts from this many ticks in powder snow: vanilla's
/// "fully frozen", and the outline's full opacity.
constexpr i32 kFullyFrozen = 140;

/// The seed of the shaking hearts, per tick (docs/provenance/hud.md § cœurs).
constexpr i32 kShakeSeed = 312871;

void icon(Gui& gui, const HudTextures& textures, f32 x, f32 y, f32 u, f32 v) {
    gui.blit(textures.icons, x, y, kIcon, kIcon, u, v, kIcon, kIcon, kSheet, kSheet);
}

[[nodiscard]] i32 ceil_int(f64 value) {
    return static_cast<i32>(std::ceil(value));
}

/// The float version: vanilla's Mth.ceil of a float, and no promotion.
[[nodiscard]] i32 ceil_int(f32 value) {
    return static_cast<i32>(std::ceil(value));
}

}  // namespace

// ── java.util.Random ────────────────────────────────────────────────────────

void JavaRandom::set_seed(i64 seed) noexcept {
    seed_ = (static_cast<u64>(seed) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
}

i32 JavaRandom::next(i32 bits) noexcept {
    seed_ = (seed_ * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
    return static_cast<i32>(static_cast<u32>(seed_ >> (48 - bits)));
}

i32 JavaRandom::next_int(i32 bound) noexcept {
    if (bound <= 0) {
        return 0;
    }
    if ((bound & -bound) == bound) {
        return static_cast<i32>((static_cast<i64>(bound) * static_cast<i64>(next(31))) >> 31);
    }
    i32 bits  = 0;
    i32 value = 0;
    do {
        bits  = next(31);
        value = bits % bound;
        // Java's `bits - val + (bound - 1) < 0`: an int overflow, here widened.
    } while (static_cast<i64>(bits) - value + (bound - 1) > std::numeric_limits<i32>::max());
    return value;
}

// ── The blink ───────────────────────────────────────────────────────────────

void HealthBlink::update(i32 health, i64 tick, i64 now_ms) noexcept {
    if (last_ < 0) {
        last_           = health;
        display_        = health;
        last_change_ms_ = now_ms;
        return;
    }
    if (health < last_) {
        last_change_ms_ = now_ms;
        blink_until_    = tick + 20;
    } else if (health > last_) {
        last_change_ms_ = now_ms;
        blink_until_    = tick + 10;
    }
    if (now_ms - last_change_ms_ > 1000) {
        display_        = health;
        last_change_ms_ = now_ms;
    }
    last_ = health;
}

HeartKind HudState::heart_kind() const noexcept {
    if (poisoned) {
        return HeartKind::Poisoned;
    }
    if (withered) {
        return HeartKind::Withered;
    }
    if (frozen_ticks >= kFullyFrozen) {
        return HeartKind::Frozen;
    }
    return HeartKind::Normal;
}

f32 heart_sprite_x(bool container, HeartKind kind, bool absorbing, bool half, bool blink) noexcept {
    i32 index = 0;
    i32 shift = 0;
    if (container) {
        shift = blink ? 1 : 0;
    } else {
        bool can_blink = true;
        if (absorbing) {
            index     = 8;
            can_blink = false;
        } else {
            switch (kind) {
                case HeartKind::Normal: index = 2; break;
                case HeartKind::Poisoned: index = 4; break;
                case HeartKind::Withered: index = 6; break;
                case HeartKind::Frozen:
                    index     = 9;
                    can_blink = false;
                    break;
            }
        }
        shift = (half ? 1 : 0) + (can_blink && blink ? 2 : 0);
    }
    return 16.0F + static_cast<f32>((index * 2 + shift) * 9);
}

HeartLayout heart_layout(f32 max_health, f32 health, i32 display_health, f32 absorption) noexcept {
    const i32 current  = ceil_int(health);
    const f32 shown    = std::max(max_health, static_cast<f32>(std::max(display_health, current)));
    const i32 absorbed = ceil_int(absorption);
    HeartLayout layout;
    layout.health_hearts     = ceil_int(static_cast<f64>(shown) / 2.0);
    layout.absorption_hearts = ceil_int(static_cast<f64>(absorbed) / 2.0);
    layout.rows              = ceil_int((shown + static_cast<f32>(absorbed)) / 2.0F / 10.0F);
    layout.row_height        = std::max(10 - (layout.rows - 2), 3);
    return layout;
}

i32 regeneration_heart(i64 tick, f32 max_health) noexcept {
    const i32 period = ceil_int(max_health + 5.0F);
    return period > 0 ? static_cast<i32>(tick % period) : -1;
}

// ── Armour points, the fallback ─────────────────────────────────────────────

i32 armour_points(std::string_view item) noexcept {
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

namespace {

/// The hearts, the armour, the haunches or the mount's hearts, the bubbles:
/// what vanilla draws when the player can be hurt.
void draw_status(Gui& gui, const HudTextures& textures, const HudState& state, f32 left, f32 right,
                 f32 top) {
    JavaRandom random(static_cast<i64>(static_cast<i32>(
        static_cast<u32>(static_cast<i32>(state.tick)) * static_cast<u32>(kShakeSeed))));

    const i32         health     = ceil_int(state.health);
    const i32         absorption = ceil_int(state.absorption);
    const HeartLayout layout =
        heart_layout(state.max_health, state.health, state.display_health, state.absorption);
    const f32 row_height = static_cast<f32>(layout.row_height);
    const i32 regen      = state.regenerating
                               ? regeneration_heart(state.tick,
                                                    std::max(state.max_health,
                                                             static_cast<f32>(std::max(
                                                                 state.display_health, health))))
                               : -1;

    // Armour, a row above the top row of hearts.
    const f32 armour_y = top - static_cast<f32>(layout.rows - 1) * row_height - 10.0F;
    for (i32 i = 0; i < 10 && state.armour > 0; ++i) {
        const f32 x     = left + static_cast<f32>(i) * 8.0F;
        const i32 level = i * 2 + 1;
        if (level < state.armour) {
            icon(gui, textures, x, armour_y, kArmourFull, kArmourRow);
        } else if (level == state.armour) {
            icon(gui, textures, x, armour_y, kArmourHalf, kArmourRow);
        } else {
            icon(gui, textures, x, armour_y, kArmourEmpty, kArmourRow);
        }
    }

    // Hearts, last to first: the random draws are made in that order.
    const HeartKind kind  = state.heart_kind();
    const f32       v     = state.hardcore ? kHardcoreHeartRow : kHeartRow;
    const bool      shake = health + absorption <= 4;
    const i32       total = layout.health_hearts + layout.absorption_hearts;
    for (i32 i = total - 1; i >= 0; --i) {
        const f32 x = left + static_cast<f32>(i % 10) * 8.0F;
        f32       y = top - static_cast<f32>(i / 10) * row_height;
        if (shake) {
            y += static_cast<f32>(random.next_int(2));
        }
        if (i < layout.health_hearts && i == regen) {
            y -= 2.0F;
        }
        gui.blit(textures.icons, x, y, kIcon, kIcon,
                 heart_sprite_x(true, kind, false, false, state.blinking), v, kIcon, kIcon, kSheet,
                 kSheet);
        const i32 two = i * 2;
        if (i >= layout.health_hearts) {
            const i32 absorbed = two - layout.health_hearts * 2;
            if (absorbed < absorption) {
                // A withered player's absorption hearts are withered too.
                const bool withered = kind == HeartKind::Withered;
                gui.blit(textures.icons, x, y, kIcon, kIcon,
                         heart_sprite_x(false, kind, !withered, absorbed + 1 == absorption, false),
                         v, kIcon, kIcon, kSheet, kSheet);
            }
            continue;
        }
        if (state.blinking && two < state.display_health) {
            gui.blit(textures.icons, x, y, kIcon, kIcon,
                     heart_sprite_x(false, kind, false, two + 1 == state.display_health, true), v,
                     kIcon, kIcon, kSheet, kSheet);
        }
        if (two < health) {
            gui.blit(textures.icons, x, y, kIcon, kIcon,
                     heart_sprite_x(false, kind, false, two + 1 == health, false), v, kIcon, kIcon,
                     kSheet, kSheet);
        }
    }

    // The right-hand column: a mount's hearts, or the haunches.
    i32 mount_hearts = 0;
    if (state.vehicle_health) {
        mount_hearts = std::min(static_cast<i32>(state.vehicle_max_health + 0.5F) / 2, 30);
    }
    if (mount_hearts == 0) {
        for (i32 i = 0; i < 10; ++i) {
            f32 y = top;
            if (state.saturation <= 0.0F && state.tick % (state.food * 3 + 1) == 0) {
                y += static_cast<f32>(random.next_int(3) - 1);
            }
            const f32 x     = right - static_cast<f32>(i) * 8.0F - kIcon;
            const i32 level = i * 2 + 1;
            icon(gui, textures, x, y, state.hunger ? kHungerEmpty : kFoodEmpty, kFoodRow);
            const f32 shift = state.hunger ? kHungerShift : 0.0F;
            if (level < state.food) {
                icon(gui, textures, x, y, kFoodFull + shift, kFoodRow);
            } else if (level == state.food) {
                icon(gui, textures, x, y, kFoodHalf + shift, kFoodRow);
            }
        }
    } else {
        const i32 mount_health = ceil_int(*state.vehicle_health);
        i32       left_hearts  = mount_hearts;
        f32       y            = top;
        i32       drawn        = 0;
        while (left_hearts > 0) {
            const i32 count = std::min(left_hearts, 10);
            left_hearts -= count;
            for (i32 i = 0; i < count; ++i) {
                const f32 x     = right - static_cast<f32>(i) * 8.0F - kIcon;
                const i32 level = i * 2 + 1 + drawn;
                icon(gui, textures, x, y, kMountContainer, kArmourRow);
                if (level < mount_health) {
                    icon(gui, textures, x, y, kMountFull, kArmourRow);
                } else if (level == mount_health) {
                    icon(gui, textures, x, y, kMountHalf, kArmourRow);
                }
            }
            y -= 10.0F;
            drawn += 20;
        }
    }

    // Bubbles, over the right-hand column.
    const i32 air = std::min(state.air, state.max_air);
    if (air < state.max_air && state.max_air > 0) {
        const i32 rows = mount_hearts > 0 ? (mount_hearts + 9) / 10 : 1;
        const f32 y    = top - 10.0F - static_cast<f32>(rows - 1) * 10.0F;
        const f64 max  = static_cast<f64>(state.max_air);
        const i32 full = ceil_int(static_cast<f64>(air - 2) * 10.0 / max);
        const i32 popping = ceil_int(static_cast<f64>(air) * 10.0 / max) - full;
        for (i32 i = 0; i < full + popping; ++i) {
            icon(gui, textures, right - static_cast<f32>(i) * 8.0F - kIcon, y,
                 i < full ? kBubbleFull : kBubblePopping, kBubbleRow);
        }
    }
}

}  // namespace

void draw_hud(Gui& gui, const ItemRenderer& items, const HudTextures& textures,
              const HudState& state) {
    const f32 width  = gui.width();
    const f32 height = gui.height();
    // Vanilla's integer half: screenWidth / 2 with screenWidth an int.
    const f32 centre = std::floor(width * 0.5F);
    const f32 left   = centre - 91.0F;
    const f32 right  = centre + 91.0F;

    // ── Powder snow: the outline over everything, as opaque as the freezing ──
    if (state.frozen_ticks > 0 && textures.powder_snow != GuiTexture::Invalid) {
        const f32  frozen = std::min(1.0F, static_cast<f32>(state.frozen_ticks) /
                                               static_cast<f32>(kFullyFrozen));
        const auto alpha  = static_cast<u32>(std::lround(frozen * 255.0F));
        gui.quad(textures.powder_snow, 0.0F, 0.0F, width, height, 0.0F, 0.0F, 1.0F, 1.0F,
                 (alpha << 24) | 0xFFFFFFU);
    }

    draw_crosshair(gui, textures);

    if (state.spectator) {
        return;  // no hotbar, no bars: the spectator menu is its own thing
    }

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

    // ── The jump bar or the experience bar ──────────────────────────────────
    //
    // Creative has neither hearts, haunches nor experience: a creative HUD with
    // ten full hearts is showing a number nothing is maintaining.
    const f32 bar_y = height - 29.0F;
    if (state.jumping_mount) {
        gui.blit(textures.icons, left, bar_y, kBarW, kBarH, kBarX, kJumpBackY, kBarW, kBarH,
                 kSheet, kSheet);
        const f32 filled = std::floor(std::clamp(state.jump, 0.0F, 1.0F) * (kBarW + 1.0F));
        if (filled > 0.0F) {
            gui.blit(textures.icons, left, bar_y, filled, kBarH, kBarX, kJumpFrontY, filled, kBarH,
                     kSheet, kSheet);
        }
    } else if (!state.creative) {
        gui.blit(textures.icons, left, bar_y, kBarW, kBarH, kBarX, kExperienceBackY, kBarW, kBarH,
                 kSheet, kSheet);
        const f32 filled =
            std::floor(std::clamp(state.experience_bar, 0.0F, 1.0F) * (kBarW + 1.0F));
        if (filled > 0.0F) {
            gui.blit(textures.icons, left, bar_y, filled, kBarH, kBarX, kExperienceFrontY, filled,
                     kBarH, kSheet, kSheet);
        }
        if (state.experience_level > 0) {
            // Outlined rather than shadowed: the number four times in black,
            // one pixel out each way, then once in green. Placed as vanilla
            // places it, (width − text) / 2 in integers.
            const std::string text = std::to_string(state.experience_level);
            const f32         x = std::floor((std::floor(width) - gui.font().width(text)) / 2.0F);
            const f32         y = height - 35.0F;
            for (const auto& [dx, dy] : std::array<std::pair<f32, f32>, 4>{
                     std::pair{1.0F, 0.0F}, {-1.0F, 0.0F}, {0.0F, 1.0F}, {0.0F, -1.0F}}) {
                (void)gui.text(x + dx, y + dy, text, 0xFF000000U, false);
            }
            (void)gui.text(x, y, text, kExperienceGreen, false);
        }
    }

    if (!state.creative) {
        draw_status(gui, textures, state, left, right, height - 39.0F);
    }

    // ── The held item's name ────────────────────────────────────────────────
    if (state.name_flash_life > 0.0F && !state.name_flash.empty()) {
        // Vanilla holds the name for forty ticks and fades the last ten, so the
        // alpha is the life clamped to the last quarter and scaled back up.
        const f32 alpha = std::clamp(state.name_flash_life * 4.0F, 0.0F, 1.0F);
        const auto byte = static_cast<u32>(std::lround(alpha * 255.0F));
        if (byte > 0) {
            const f32 y = height - 59.0F + (state.creative ? 14.0F : 0.0F);
            (void)gui.text_centred(centre, y, state.name_flash, (byte << 24) | 0xFFFFFFU, true);
        }
    }
}

}  // namespace ov::client
