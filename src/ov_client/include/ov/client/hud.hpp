// The heads-up display: everything drawn over the world and under a screen.
//
// Nothing here decides anything. Every number in HudState arrives from the
// server — Set Health, Set Experience, Set Container Content — and the HUD's
// whole job is to lay them out where 1.20.1 lays them out. That is deliberate:
// a HUD that computes its own hearts is a HUD that disagrees with the server
// the first time something unexpected happens, and the player believes the
// HUD.
//
// The sprite coordinates below were **measured from the pack's own textures**,
// not remembered: scripts/measure_gui_sprites.py walks `icons.png` cell by
// cell and reports what is in each, which is how the gold hearts at (160, 0)
// and the olive poisoned ones at (88, 0) were told apart. See
// docs/provenance/interface.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/item_view.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace ov::client {

/// Everything the HUD draws, and where it comes from.
struct HudState {
    /// Set Health. Twenty is ten hearts.
    f32 health{20.0F};
    f32 max_health{20.0F};
    /// ⚠️ Never fed. Absorption is entity metadata index 15 on the player's own
    /// entity, and this server does not send metadata for the player it is
    /// talking to. The row is drawn when it is non-zero, and it never is —
    /// stated here rather than left as a silently dead branch.
    f32 absorption{0.0F};

    /// Set Health again: the haunches.
    i32 food{20};
    f32 saturation{5.0F};

    /// Set Experience.
    f32 experience_bar{0.0F};
    i32 experience_level{0};

    /// Armour points, 0 to 20.
    ///
    /// ⚠️ Computed on this side from what is in the armour slots, because
    /// nothing sends it: vanilla puts it in the `generic.armor` attribute and
    /// this server sends no attributes. See armour_points().
    i32 armour{0};

    /// Air, 0 to 300. ⚠️ Never fed either, and for the same reason: the server
    /// computes it (survival_session.cpp does the drowning) and never puts it
    /// on the wire. The bubbles are implemented and will light up the day it
    /// does.
    i32 air{300};
    i32 max_air{300};

    /// The hotbar, in window-0 slots 36..44.
    std::array<ItemStackView, 9> hotbar{};
    i32                          selected{0};
    /// Slot 45 of window 0. Drawn beside the hotbar when it holds something.
    ItemStackView off_hand{};

    /// The name that floats over the hotbar after the selection changes, and
    /// how much of its life is left, 1 down to 0. Vanilla holds it for forty
    /// ticks and fades the last ten.
    std::string name_flash;
    f32         name_flash_life{0.0F};

    /// Creative hides the hearts, the haunches and the experience bar, and
    /// moves the hotbar's contents up. Nothing else changes.
    bool creative{false};

    /// A red tint over the hearts for the ten ticks after damage. Fed from the
    /// health going down, because that is the only damage signal this client
    /// receives.
    f32 damage_flash{0.0F};
};

/// Armour points for one item, by name.
///
/// ⚠️ **Not measured.** These are the game's published per-piece values and
/// there is no oracle to check them against here: the server sends no armour
/// attribute, and the data generator emits no item attributes either. Every
/// other number in this file came out of a texture or off the wire; this table
/// did not, and saying so is the point.
[[nodiscard]] i32 armour_points(std::string_view item) noexcept;

/// The sum over the four armour slots.
[[nodiscard]] i32 armour_points(std::span<const ItemStackView> armour_slots) noexcept;

/// The two sheets the HUD draws from, already registered with the Gui.
struct HudTextures {
    /// `gui/widgets`: the hotbar and its selection.
    GuiTexture widgets{GuiTexture::Invalid};
    /// `gui/icons`: hearts, haunches, armour, bubbles, the experience bar and
    /// the crosshair.
    GuiTexture icons{GuiTexture::Invalid};
};

/// Draw the whole HUD. Between gui.begin() and gui.flush().
void draw_hud(Gui& gui, const ItemRenderer& items, const HudTextures& textures,
              const HudState& state);

/// Draw vanilla's crosshair sprite, `icons.png` at (0, 0), fifteen by fifteen.
///
/// ⚠️ Vanilla *inverts* what is under it, so it is visible on any background.
/// This draws white at three quarters instead, because inversion needs a blend
/// mode the RHI does not have. Same compromise the line crosshair in
/// overlay.cpp already makes, and the same note.
void draw_crosshair(Gui& gui, const HudTextures& textures);

}  // namespace ov::client
