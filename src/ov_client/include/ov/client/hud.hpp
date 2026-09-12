// The heads-up display: everything drawn over the world and under a screen.
//
// Nothing here decides anything. Every number in HudState arrives from the
// server — Set Health, Set Experience, Set Container Content, the player's own
// metadata (air, frozen ticks, absorption), Entity Effect, Update Attributes —
// and the HUD's whole job is to lay them out where 1.20.1 lays them out.
//
// **Where** was measured on the real 1.20.1 client, not remembered:
// scripts/measure_hud.py makes it and ours play the same scenes against the
// same vanilla server and scripts/compare_hud.py holds the two captures pixel
// against pixel (docs/provenance/hud.md). The sprite cells were read off the
// pack's own `icons.png` by scripts/measure_gui_sprites.py
// (docs/provenance/interface.md).
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/client/item_view.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ov::client {

/// What colour the hearts are. Absorption is not here: it is its own hearts,
/// after these.
enum class HeartKind : u8 { Normal, Poisoned, Withered, Frozen };

/// java.util.Random, as the JDK's API documentation specifies it: a 48-bit
/// linear congruential generator (multiplier 0x5DEECE66D, addend 0xB), and
/// nextInt(bound) as documented there, rejection loop included. The HUD's
/// shaking hearts draw from one reseeded every frame by the tick count; the
/// seed and the draw order are what docs/provenance/hud.md measured.
class JavaRandom {
public:
    explicit JavaRandom(i64 seed = 0) noexcept { set_seed(seed); }
    void               set_seed(i64 seed) noexcept;
    [[nodiscard]] i32  next(i32 bits) noexcept;
    [[nodiscard]] i32  next_int(i32 bound) noexcept;

private:
    u64 seed_{0};
};

/// The hearts' blink after a change of health, kept as vanilla's Gui keeps
/// it: the health shown (`display`) lags a real second behind a drop, and the
/// hearts blink for twenty ticks after one and ten after a rise.
class HealthBlink {
public:
    /// Every frame, with the health rounded up, the GUI's tick and the wall
    /// clock in milliseconds.
    void update(i32 health, i64 tick, i64 now_ms) noexcept;

    [[nodiscard]] bool blinking(i64 tick) const noexcept {
        return blink_until_ > tick && (blink_until_ - tick) / 3 % 2 == 1;
    }
    [[nodiscard]] i32 display_health() const noexcept { return display_; }
    [[nodiscard]] i64 blink_until() const noexcept { return blink_until_; }

private:
    i32  last_{-1};
    i32  display_{0};
    i64  last_change_ms_{0};
    i64  blink_until_{0};
};

/// Everything the HUD draws, and where it comes from.
struct HudState {
    /// Set Health. Twenty is ten hearts.
    f32 health{20.0F};
    /// The `generic.max_health` attribute; twenty unless Update Attributes
    /// says otherwise.
    f32 max_health{20.0F};
    /// Metadata 15 on the player's own entity.
    f32 absorption{0.0F};

    /// Set Health again: the haunches.
    i32 food{20};
    f32 saturation{5.0F};

    /// Set Experience.
    f32 experience_bar{0.0F};
    i32 experience_level{0};

    /// Armour points, 0 to 20: the `generic.armor` attribute when the server
    /// sent it (vanilla does), else armour_points() over the armour slots.
    i32 armour{0};

    /// Metadata 1 (air) and 7 (frozen ticks) on the player's own entity.
    i32 air{300};
    i32 max_air{300};
    i32 frozen_ticks{0};

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

    /// Creative and spectator have no hearts, haunches or experience bar;
    /// spectator has no hotbar either.
    bool creative{false};
    bool spectator{false};
    /// Login (play)'s flag: the hardcore row of every heart.
    bool hardcore{false};

    /// The GUI's tick, at 20 Hz, and the wall clock: the blink, the
    /// regeneration wave and the shaking hearts are functions of these.
    i64 tick{0};
    /// The blink of the hearts, maintained by the owner with HealthBlink.
    bool blinking{false};
    i32  display_health{20};

    /// From the effects: poison and wither colour the hearts, regeneration
    /// sends a wave through them, hunger turns the haunches green.
    bool poisoned{false};
    bool withered{false};
    bool regenerating{false};
    bool hunger{false};

    /// A living mount: its hearts replace the haunches.
    std::optional<f32> vehicle_health;
    f32                vehicle_max_health{0.0F};
    /// A mount that jumps (a horse): the jump bar replaces the experience
    /// bar. `jump` is the charge, 0 to 1.
    bool jumping_mount{false};
    f32  jump{0.0F};

    /// Frozen hearts at 140 ticks — vanilla's "fully frozen".
    [[nodiscard]] HeartKind heart_kind() const noexcept;
};

/// Armour points for one item, by name. The fallback when no `generic.armor`
/// attribute has arrived.
///
/// ⚠️ **Not measured.** These are the game's published per-piece values; the
/// data generator emits no item attributes. The attribute, when it comes, is
/// the measured truth and wins.
[[nodiscard]] i32 armour_points(std::string_view item) noexcept;

/// The sum over the four armour slots.
[[nodiscard]] i32 armour_points(std::span<const ItemStackView> armour_slots) noexcept;

/// The sheets the HUD draws from, already registered with the Gui.
struct HudTextures {
    /// `gui/widgets`: the hotbar and its selection.
    GuiTexture widgets{GuiTexture::Invalid};
    /// `gui/icons`: hearts, haunches, armour, bubbles, the experience bar, the
    /// jump bar and the crosshair.
    GuiTexture icons{GuiTexture::Invalid};
    /// `misc/powder_snow_outline`, stretched over the screen while freezing.
    GuiTexture powder_snow{GuiTexture::Invalid};
};

/// The heart row's sprite column, for one heart. `half` and `blink` as the
/// sheet lays them out: container 16/25, normal 52, poisoned 88, withered 124,
/// absorbing 160, frozen 178; half +9, blinking +18 (not absorbing, frozen or
/// the container's half).
[[nodiscard]] f32 heart_sprite_x(bool container, HeartKind kind, bool absorbing, bool half,
                                 bool blink) noexcept;

/// Where the hearts are: the rows the health and absorption hearts take, and
/// the step between two rows, 10 down to 3 as the rows multiply.
struct HeartLayout {
    i32 health_hearts{10};
    i32 absorption_hearts{0};
    i32 rows{1};
    i32 row_height{10};
};
[[nodiscard]] HeartLayout heart_layout(f32 max_health, f32 health, i32 display_health,
                                       f32 absorption) noexcept;

/// The heart raised by the regeneration wave at `tick`: tick modulo
/// ceil(max health + 5). Past the last heart, none is.
[[nodiscard]] i32 regeneration_heart(i64 tick, f32 max_health) noexcept;

/// The x of an effect icon's 24×24 frame, and its alpha when it is about to
/// run out, kept here so the tests reach them. See status_effects.hpp.

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
