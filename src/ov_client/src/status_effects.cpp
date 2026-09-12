#include "ov/client/status_effects.hpp"

#include "ov/render/texture_image.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ov::client {
namespace {

constexpr f32 kSheet      = 256.0F;
constexpr f32 kFrame      = 24.0F;
constexpr f32 kFrameX     = 141.0F;
constexpr f32 kAmbientX   = 165.0F;
constexpr f32 kFrameY     = 166.0F;
constexpr f32 kIcon       = 18.0F;
constexpr f32 kStep       = 25.0F;
constexpr f32 kHarmfulRow = 26.0F;

}  // namespace

bool effect_is_beneficial(std::string_view name) noexcept {
    // The Minecraft Wiki, "Effect", 1.20.1: the nineteen beneficial effects.
    // Harmful and neutral ones fall in the other row.
    static constexpr std::array<std::string_view, 19> kBeneficial{
        "minecraft:speed",          "minecraft:haste",          "minecraft:strength",
        "minecraft:instant_health", "minecraft:jump_boost",     "minecraft:regeneration",
        "minecraft:resistance",     "minecraft:fire_resistance", "minecraft:water_breathing",
        "minecraft:invisibility",   "minecraft:night_vision",   "minecraft:health_boost",
        "minecraft:absorption",     "minecraft:saturation",     "minecraft:luck",
        "minecraft:slow_falling",   "minecraft:conduit_power",  "minecraft:dolphins_grace",
        "minecraft:hero_of_the_village",
    };
    return std::ranges::find(kBeneficial, name) != kBeneficial.end();
}

void sort_effects(std::vector<HudEffect>& effects) {
    // Measured on the real client's 11-effects capture (docs/provenance/hud.md
    // § 4): nearest the right edge the longest remaining time, and an
    // infinite effect counts as the longest of all — luck (infinite),
    // strength 568, speed 567, night vision 150; weakness 574, slowness 572.
    // Not measured: ambient effects after the others, the swirl colour as the
    // tie-break.
    const auto key = [](const HudEffect& e) {
        return e.duration < 0 ? std::numeric_limits<i32>::max() : e.duration;
    };
    std::ranges::stable_sort(effects, [&](const HudEffect& a, const HudEffect& b) {
        if (a.ambient != b.ambient) {
            return !a.ambient;
        }
        if (key(a) != key(b)) {
            return key(a) > key(b);
        }
        return a.color > b.color;
    });
}

f32 effect_icon_alpha(i32 duration, bool ambient) noexcept {
    if (ambient || duration < 0 || duration > 200) {
        return 1.0F;
    }
    const auto d        = static_cast<f32>(duration);
    const f32  base     = std::clamp(d / 10.0F / 5.0F * 0.5F, 0.0F, 0.5F);
    const f32  swing    = std::clamp(static_cast<f32>(10 - duration / 20) / 10.0F * 0.25F, 0.0F, 0.25F);
    return base + std::cos(d * std::numbers::pi_v<f32> / 5.0F) * swing;
}

render::TextureImage pack_effect_icons(std::span<const std::string>        names,
                                       std::span<const render::TextureImage> images,
                                       std::unordered_map<std::string, std::array<f32, 4>>& cells) {
    render::TextureImage out;
    if (images.empty() || images.size() != names.size()) {
        return out;
    }
    const u32 cell    = images.front().width;
    const u32 columns = 8;
    const u32 rows    = static_cast<u32>((images.size() + columns - 1) / columns);
    out.width         = cell * columns;
    out.height        = cell * rows;
    out.rgba.assign(static_cast<usize>(out.width) * out.height * 4, 0);
    for (usize i = 0; i < images.size(); ++i) {
        const render::TextureImage& image = images[i];
        if (image.width != cell || image.height < cell) {
            continue;  // an animated strip keeps its first frame below
        }
        const u32 ox = static_cast<u32>(i % columns) * cell;
        const u32 oy = static_cast<u32>(i / columns) * cell;
        for (u32 y = 0; y < cell; ++y) {
            for (u32 x = 0; x < cell; ++x) {
                const usize from = image.index(x, y);
                const usize to   = out.index(ox + x, oy + y);
                std::copy_n(image.rgba.begin() + static_cast<isize>(from), 4,
                            out.rgba.begin() + static_cast<isize>(to));
            }
        }
        cells[names[i]] = {static_cast<f32>(ox) / static_cast<f32>(out.width),
                           static_cast<f32>(oy) / static_cast<f32>(out.height),
                           static_cast<f32>(ox + cell) / static_cast<f32>(out.width),
                           static_cast<f32>(oy + cell) / static_cast<f32>(out.height)};
    }
    return out;
}

void draw_status_effects(Gui& gui, const EffectTextures& textures,
                         std::span<const HudEffect> effects) {
    if (textures.frames == GuiTexture::Invalid) {
        return;
    }
    i32 beneficial = 0;
    i32 harmful    = 0;
    struct Placed {
        f32  x;
        f32  y;
        f32  alpha;
        const HudEffect* effect;
    };
    std::array<Placed, 40> placed{};
    usize                  count = 0;
    for (const HudEffect& effect : effects) {
        if (!effect.show_icon || count == placed.size()) {
            continue;
        }
        f32 x = gui.width();
        f32 y = 1.0F;
        if (effect.beneficial) {
            ++beneficial;
            x -= kStep * static_cast<f32>(beneficial);
        } else {
            ++harmful;
            x -= kStep * static_cast<f32>(harmful);
            y += kHarmfulRow;
        }
        gui.blit(textures.frames, x, y, kFrame, kFrame, effect.ambient ? kAmbientX : kFrameX,
                 kFrameY, kFrame, kFrame, kSheet, kSheet);
        placed[count++] = Placed{x, y, effect_icon_alpha(effect.duration, effect.ambient), &effect};
    }
    // The sprites after every frame: one texture change, not one per icon.
    if (textures.icons == nullptr || textures.icons->texture == GuiTexture::Invalid) {
        return;
    }
    for (usize i = 0; i < count; ++i) {
        const auto it = textures.icons->cells.find(placed[i].effect->name);
        if (it == textures.icons->cells.end()) {
            continue;
        }
        const auto alpha =
            static_cast<u32>(std::lround(std::clamp(placed[i].alpha, 0.0F, 1.0F) * 255.0F));
        const auto& uv = it->second;
        gui.quad(textures.icons->texture, placed[i].x + 3.0F, placed[i].y + 3.0F, kIcon, kIcon,
                 uv[0], uv[1], uv[2], uv[3], (alpha << 24) | 0xFFFFFFU);
    }
}

}  // namespace ov::client
