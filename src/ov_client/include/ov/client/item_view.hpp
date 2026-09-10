// One item stack, drawn into one sixteen-pixel cell.
//
// Two things are drawn and they are not the same thing. The *icon* is either a
// flat sprite or a projected block model, and ov_render decided which and did
// the projection; all that is left here is to hand the quads to the batcher.
// The *decorations* — the stack count, and one day the durability bar — are
// text and rectangles on top of it, and vanilla draws them in a second pass
// for a reason: a count under an item's own quads is unreadable.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"
#include "ov/render/item_model.hpp"

#include <array>
#include <string_view>

namespace ov::client {

/// A stack, as the interface sees it: a name it can look a model up by, and a
/// count. The name is borrowed — it points into the registry, which outlives
/// every frame.
struct ItemStackView {
    std::string_view item;
    i32              count{0};
    /// The tint of each layer as 0xRRGGBB, -1 for none — what the real
    /// client's item colour handlers answered (render::CreativeItemInfo).
    /// Without them a tinted face falls back to the one foliage colour the
    /// renderer was built with, and a flat layer is drawn untinted.
    std::array<i32, 3> tints{-1, -1, -1};
    bool               has_tints{false};
    /// Durability: the bar is drawn when both are positive.
    i32 damage{0};
    i32 max_damage{0};

    [[nodiscard]] bool empty() const noexcept { return item.empty() || count <= 0; }
};

/// Vanilla's durability bar for a damaged stack: its width in pixels (0..13)
/// and its colour, 0xRRGGBB. The hue runs from green at full health to red
/// at none, a third of the colour wheel.
struct DurabilityBar {
    i32 width{0};
    u32 rgb{0};
};

[[nodiscard]] DurabilityBar durability_bar(i32 damage, i32 max_damage) noexcept;

/// Draws item stacks through a Gui.
///
/// Holds no state of its own beyond what it was built with, so it is safe to
/// keep one for the life of the client and call it from anywhere in a frame.
class ItemRenderer {
public:
    /// `atlas` is the block atlas, already registered with the Gui — the same
    /// texture the terrain draws from, because an item's sprites were stitched
    /// into it rather than into a second sheet.
    ItemRenderer(const render::ItemModelCache& models, GuiTexture atlas,
                 u32 foliage_tint) noexcept
        : models_(&models), atlas_(atlas), foliage_tint_(foliage_tint) {}

    /// Draw the icon at the cell's top-left, in GUI pixels.
    void draw(Gui& gui, f32 x, f32 y, const ItemStackView& stack) const;

    /// Draw the count over it. Separate from draw(), because every icon in a
    /// screen is drawn before any count is: it saves a batch change per slot,
    /// and it is the only way a count is never hidden by the next item's quads.
    void draw_count(Gui& gui, f32 x, f32 y, const ItemStackView& stack) const;

    /// A bare sprite of the block atlas in a cell: the silhouettes vanilla
    /// draws in an empty armour slot are atlas sprites, not items.
    void draw_sprite(Gui& gui, f32 x, f32 y, const render::SpriteUv& uv) const;

    /// True when the item resolved to something drawable. False means the icon
    /// is missing, which is worth reporting rather than showing an empty slot.
    [[nodiscard]] bool drawable(std::string_view item) const noexcept;

private:
    const render::ItemModelCache* models_{nullptr};
    GuiTexture                    atlas_{GuiTexture::Invalid};
    /// The colour a tinted face takes. Vanilla resolves this per item through
    /// a table of colour handlers; here it is one value, which is right for
    /// grass and leaves and wrong for nothing else the lab bench holds.
    u32 foliage_tint_{0xFFFFFF};
};

}  // namespace ov::client
