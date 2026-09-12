// The status effect icons at the top right of the HUD.
//
// Measured on the real 1.20.1 client (docs/provenance/hud.md § effets): a
// 24×24 frame from `gui/container/inventory.png` — (141, 166), or (165, 166)
// for an effect from a beacon or a conduit — every 25 pixels leftwards from the
// right edge, beneficial effects on the row at y 1 and the others on the row at
// y 27; the effect's own 18×18 sprite 3 pixels in. An effect given with
// `hideParticles` carries no icon flag and is not drawn at all.
//
// Nothing here knows a packet: the owner keeps the effects (Entity Effect and
// Remove Entity Effect, counted down one tick at a time as vanilla's client
// counts them) and hands them over.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/render/texture_image.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::client {

struct HudEffect {
    /// "minecraft:speed".
    std::string name;
    /// Ticks left; -1 for infinite.
    i32  duration{0};
    i32  amplifier{0};
    bool ambient{false};
    bool show_icon{true};
    /// The effect's category is beneficial. Neutral effects (glowing, bad
    /// omen) share the harmful row, as vanilla's isBeneficial() says no.
    bool beneficial{true};
    /// The swirl colour, the last key of vanilla's ordering.
    u32 color{0};
};

/// The effect's category, from the Minecraft Wiki's "Effect" table for
/// 1.20.1 (beneficial / harmful / neutral); true for beneficial only.
[[nodiscard]] bool effect_is_beneficial(std::string_view name) noexcept;

/// The order they are drawn in, first to last — the first of each row nearest
/// the right edge. See the definition for what was measured.
void sort_effects(std::vector<HudEffect>& effects);

/// The icon's opacity: 1, except for an effect that is neither infinite nor
/// ambient in its last ten seconds, when it flickers.
[[nodiscard]] f32 effect_icon_alpha(i32 duration, bool ambient) noexcept;

/// Every effect sprite packed into one image, so that six icons cost one
/// texture bind and not six (the descriptor pool is 64 sets a frame for the
/// whole client — docs/provenance/ecrans.md § 4).
struct EffectIconAtlas {
    GuiTexture texture{GuiTexture::Invalid};
    /// u0, v0, u1, v1 in 0..1, by effect name.
    std::unordered_map<std::string, std::array<f32, 4>> cells;
};

/// Pack same-sized sprites into a grid, row by row. `names` and `images` run
/// in parallel; an image of another size than the first is skipped.
[[nodiscard]] render::TextureImage pack_effect_icons(std::span<const std::string>        names,
                                                    std::span<const render::TextureImage> images,
                                                    std::unordered_map<std::string, std::array<f32, 4>>& cells);

/// The frames' sheet, `gui/container/inventory`.
struct EffectTextures {
    GuiTexture             frames{GuiTexture::Invalid};
    const EffectIconAtlas* icons{nullptr};
};

/// Draw the icons of `effects`, already sorted. Between gui.begin() and
/// gui.flush().
void draw_status_effects(Gui& gui, const EffectTextures& textures,
                         std::span<const HudEffect> effects);

}  // namespace ov::client
