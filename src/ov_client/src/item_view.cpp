#define OV_LOG_CATEGORY "client"

#include "ov/client/item_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace ov::client {

namespace {

/// Multiply a colour by a face's shade, in sRGB.
///
/// In sRGB and not in linear, because that is where vanilla does it: its
/// vertex colours are bytes multiplied by a float and written straight into an
/// unmanaged framebuffer. Doing it in linear would give a cube whose sides are
/// visibly lighter than the game's.
[[nodiscard]] u32 shaded(u32 rgb, f32 shade) noexcept {
    const auto channel = [shade](u32 value) {
        return static_cast<u32>(std::lround(std::clamp(static_cast<f32>(value) * shade, 0.0F,
                                                       255.0F)));
    };
    return 0xFF000000U | (channel((rgb >> 16) & 0xFFU) << 16) |
           (channel((rgb >> 8) & 0xFFU) << 8) | channel(rgb & 0xFFU);
}

}  // namespace

bool ItemRenderer::drawable(std::string_view item) const noexcept {
    const render::ItemMesh* mesh = models_->mesh(item);
    return mesh != nullptr && mesh->drawable;
}

void ItemRenderer::draw(Gui& gui, f32 x, f32 y, const ItemStackView& stack) const {
    if (stack.empty()) {
        return;
    }
    const render::ItemMesh* mesh = models_->mesh(stack.item);
    if (mesh == nullptr || !mesh->drawable) {
        return;
    }

    const auto tint_of = [&stack](i32 index, u32 fallback) -> u32 {
        if (!stack.has_tints) {
            return fallback;
        }
        if (index < 0 || index >= static_cast<i32>(stack.tints.size()) ||
            stack.tints[static_cast<usize>(index)] < 0) {
            return 0xFFFFFFU;
        }
        return static_cast<u32>(stack.tints[static_cast<usize>(index)]);
    };

    if (mesh->flat) {
        // A flat icon is its layers, back to front, filling the cell exactly.
        // The sprite is in the block atlas, which is why an item texture had to
        // be added to the same builder the terrain's were. Layer i takes tint
        // i: a spawn egg's base and spots, a potion's liquid.
        for (usize layer = 0; layer < mesh->layers.size(); ++layer) {
            const render::SpriteUv& uv = mesh->layers[layer];
            gui.quad(atlas_, x, y, render::kItemCellSize, render::kItemCellSize, uv.u0, uv.v0,
                     uv.u1, uv.v1, 0xFF000000U | tint_of(static_cast<i32>(layer), 0xFFFFFFU));
        }
        return;
    }

    for (const render::ItemQuad& quad : mesh->quads) {
        const u32 base   = quad.tint_index >= 0 ? tint_of(quad.tint_index, foliage_tint_) : 0xFFFFFFU;
        const u32 colour = shaded(base, quad.shade);

        const std::array<GuiPoint, 4> corners{
            GuiPoint{x + quad.position[0].x, y + quad.position[0].y},
            GuiPoint{x + quad.position[1].x, y + quad.position[1].y},
            GuiPoint{x + quad.position[2].x, y + quad.position[2].y},
            GuiPoint{x + quad.position[3].x, y + quad.position[3].y}};
        const std::array<GuiPoint, 4> uvs{
            GuiPoint{quad.uv[0].x, quad.uv[0].y}, GuiPoint{quad.uv[1].x, quad.uv[1].y},
            GuiPoint{quad.uv[2].x, quad.uv[2].y}, GuiPoint{quad.uv[3].x, quad.uv[3].y}};
        gui.quad_corners(atlas_, corners, uvs, colour);
    }
}

DurabilityBar durability_bar(i32 damage, i32 max_damage) noexcept {
    if (max_damage <= 0 || damage <= 0) {
        return {};
    }
    const f32 d   = static_cast<f32>(std::min(damage, max_damage));
    const f32 max = static_cast<f32>(max_damage);
    DurabilityBar bar;
    bar.width = static_cast<i32>(std::lround(13.0F - d * 13.0F / max));
    // HSV with S = V = 1: hue from 1/3 (green) down to 0 (red).
    const f32 hue = std::max(0.0F, (max - d) / max) / 3.0F;
    const f32 h6  = hue * 6.0F;
    const i32 sector = static_cast<i32>(std::floor(h6)) % 6;
    const f32 f      = h6 - std::floor(h6);
    const auto byte  = [](f32 v) { return static_cast<u32>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
    switch (sector) {
        case 0: r = 1.0F; g = f; break;
        case 1: r = 1.0F - f; g = 1.0F; break;
        case 2: g = 1.0F; b = f; break;
        case 3: g = 1.0F - f; b = 1.0F; break;
        case 4: r = f; b = 1.0F; break;
        default: r = 1.0F; b = 1.0F - f; break;
    }
    bar.rgb = (byte(r) << 16) | (byte(g) << 8) | byte(b);
    return bar;
}

void ItemRenderer::draw_count(Gui& gui, f32 x, f32 y, const ItemStackView& stack) const {
    if (stack.empty()) {
        return;
    }
    if (stack.count > 1) {
        // Vanilla's placement: right-aligned two pixels in from the cell's
        // right edge, and nine pixels down from its top.
        const std::string text = std::to_string(stack.count);
        (void)gui.text_right(x + 17.0F, y + 9.0F, text, 0xFFFFFFFFU, true);
    }
    // The durability bar, after the count as vanilla orders its decorations:
    // two black rows thirteen wide, two pixels in and thirteen down, and the
    // coloured top row over them.
    if (stack.max_damage > 0 && stack.damage > 0) {
        const DurabilityBar bar = durability_bar(stack.damage, stack.max_damage);
        gui.fill(x + 2.0F, y + 13.0F, 13.0F, 2.0F, 0xFF000000U);
        gui.fill(x + 2.0F, y + 13.0F, static_cast<f32>(bar.width), 1.0F, 0xFF000000U | bar.rgb);
    }
}

void ItemRenderer::draw_sprite(Gui& gui, f32 x, f32 y, const render::SpriteUv& uv) const {
    gui.quad(atlas_, x, y, render::kItemCellSize, render::kItemCellSize, uv.u0, uv.v0, uv.u1,
             uv.v1, 0xFFFFFFFFU);
}

}  // namespace ov::client
