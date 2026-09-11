#include "ov/client/menu_widgets.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {
namespace {

constexpr f32 kSheet = 256.0F;

/// A sheet rect stretched to any width by its two halves: the left `w/2`
/// columns from the left edge of the source, the rest from its right edge.
/// Vanilla's buttons have a uniform middle, so this is pixel for pixel what a
/// wider or narrower button looks like.
void blit_halves(Gui& gui, GuiTexture texture, const Rect& rect, f32 u, f32 v, f32 source_w,
                 f32 source_h, u32 argb = 0xFFFFFFFFU) {
    const f32 left  = std::floor(rect.w * 0.5F);
    const f32 right = rect.w - left;
    gui.blit(texture, rect.x, rect.y, left, rect.h, u, v, left, source_h, kSheet, kSheet, argb);
    gui.blit(texture, rect.x + left, rect.y, right, rect.h, u + source_w - right, v, right,
             source_h, kSheet, kSheet, argb);
}

}  // namespace

const Widget* widget_at(std::span<const Widget> widgets, f32 x, f32 y) noexcept {
    for (const Widget& widget : widgets) {
        if (widget.kind != WidgetKind::Label && widget.rect.contains(x, y)) {
            return &widget;
        }
    }
    return nullptr;
}

Widget* find_widget(std::span<Widget> widgets, std::string_view id) noexcept {
    for (Widget& widget : widgets) {
        if (widget.id == id) {
            return &widget;
        }
    }
    return nullptr;
}

void draw_button(Gui& gui, const MenuTextures& textures, const Rect& rect, std::string_view text,
                 bool active, bool hovered, u32 colour) {
    const f32 v = !active ? 46.0F : (hovered ? 86.0F : 66.0F);
    if (textures.widgets != GuiTexture::Invalid) {
        blit_halves(gui, textures.widgets, rect, 0.0F, v, 200.0F, 20.0F);
    } else {
        gui.fill(rect.x, rect.y, rect.w, rect.h, hovered ? 0xFF8090C0U : 0xFF6F6F6FU);
    }
    const u32 ink = active ? colour : 0xFFA0A0A0U;
    gui.text_centred(rect.x + rect.w * 0.5F, rect.y + std::floor((rect.h - 8.0F) * 0.5F), text,
                     ink);
}

void draw_slider(Gui& gui, const MenuTextures& textures, const Rect& rect, std::string_view text,
                 f64 value, bool hovered) {
    if (textures.slider != GuiTexture::Invalid) {
        blit_halves(gui, textures.slider, rect, 0.0F, 0.0F, 200.0F, 20.0F);
        const f32 handle_x =
            rect.x + std::floor(static_cast<f32>(std::clamp(value, 0.0, 1.0)) * (rect.w - 8.0F));
        const Rect handle{handle_x, rect.y, 8.0F, rect.h};
        blit_halves(gui, textures.slider, handle, 0.0F, hovered ? 60.0F : 40.0F, 200.0F, 20.0F);
    } else {
        gui.fill(rect.x, rect.y, rect.w, rect.h, 0xFF000000U);
    }
    gui.text_centred(rect.x + rect.w * 0.5F, rect.y + std::floor((rect.h - 8.0F) * 0.5F), text,
                     0xFFFFFFFFU);
}

void draw_edit_box(Gui& gui, const Rect& rect, std::string_view text, bool focused,
                   bool cursor_visible) {
    gui.fill(rect.x - 1.0F, rect.y - 1.0F, rect.w + 2.0F, rect.h + 2.0F,
             focused ? 0xFFFFFFFFU : 0xFFA0A0A0U);
    gui.fill(rect.x, rect.y, rect.w, rect.h, 0xFF000000U);
    const f32 text_y = rect.y + std::floor((rect.h - 8.0F) * 0.5F);
    const f32 end    = gui.text(rect.x + 4.0F, text_y, text, 0xFFE0E0E0U);
    if (focused && cursor_visible) {
        gui.text(end, text_y, "_", 0xFFE0E0E0U);
    }
}

f64 slider_value_at(const Rect& rect, f32 x) noexcept {
    if (rect.w <= 8.0F) {
        return 0.0;
    }
    return std::clamp(static_cast<f64>(x - (rect.x + 4.0F)) / static_cast<f64>(rect.w - 8.0F),
                      0.0, 1.0);
}

void draw_dirt(Gui& gui, const MenuTextures& textures, const Rect& rect, u32 tint) {
    if (textures.dirt == GuiTexture::Invalid) {
        gui.fill(rect.x, rect.y, rect.w, rect.h, 0xFF202020U);
        return;
    }
    constexpr f32 kTile = 32.0F;
    for (f32 y = rect.y; y < rect.y + rect.h; y += kTile) {
        for (f32 x = rect.x; x < rect.x + rect.w; x += kTile) {
            const f32 tw = std::min(kTile, rect.x + rect.w - x);
            const f32 th = std::min(kTile, rect.y + rect.h - y);
            gui.quad(textures.dirt, x, y, tw, th, 0.0F, 0.0F, tw / kTile, th / kTile, tint);
        }
    }
}

void draw_widgets(Gui& gui, const MenuTextures& textures, std::span<const Widget> widgets,
                  f32 mouse_x, f32 mouse_y, bool cursor_visible) {
    // Two passes: every background, then every label. Drawing each widget
    // whole alternates the sheet and a font page, and the GUI cuts a draw at
    // every texture change — the key binds screen made ~90 of them, and past
    // the RHI's per-frame texture bindings the last draws sampled the wrong
    // image (font glyphs over the Reset buttons, the first capture). Two
    // passes is a handful of draws whatever the screen.
    for (const Widget& widget : widgets) {
        const bool  hovered = widget.rect.contains(mouse_x, mouse_y);
        const Rect& rect    = widget.rect;
        switch (widget.kind) {
            case WidgetKind::Button: {
                const f32 v = !widget.active ? 46.0F : (hovered ? 86.0F : 66.0F);
                if (textures.widgets != GuiTexture::Invalid) {
                    blit_halves(gui, textures.widgets, rect, 0.0F, v, 200.0F, 20.0F);
                } else {
                    gui.fill(rect.x, rect.y, rect.w, rect.h, hovered ? 0xFF8090C0U : 0xFF6F6F6FU);
                }
                break;
            }
            case WidgetKind::Slider:
                if (textures.slider != GuiTexture::Invalid) {
                    blit_halves(gui, textures.slider, rect, 0.0F, 0.0F, 200.0F, 20.0F);
                    const f32 handle_x =
                        rect.x + std::floor(static_cast<f32>(std::clamp(widget.value, 0.0, 1.0)) *
                                            (rect.w - 8.0F));
                    blit_halves(gui, textures.slider, Rect{handle_x, rect.y, 8.0F, rect.h}, 0.0F,
                                hovered ? 60.0F : 40.0F, 200.0F, 20.0F);
                } else {
                    gui.fill(rect.x, rect.y, rect.w, rect.h, 0xFF000000U);
                }
                break;
            case WidgetKind::EditBox:
                gui.fill(rect.x - 1.0F, rect.y - 1.0F, rect.w + 2.0F, rect.h + 2.0F,
                         widget.selected ? 0xFFFFFFFFU : 0xFFA0A0A0U);
                gui.fill(rect.x, rect.y, rect.w, rect.h, 0xFF000000U);
                break;
            case WidgetKind::Tab: {
                // tab_button.png: 130×24 cells, selected rows first.
                const f32 row = (widget.selected ? 0.0F : 2.0F) + (hovered ? 1.0F : 0.0F);
                if (textures.tab != GuiTexture::Invalid) {
                    blit_halves(gui, textures.tab, rect, 0.0F, row * 24.0F, 130.0F, 24.0F);
                }
                break;
            }
            case WidgetKind::Label:
            case WidgetKind::Icon: break;
        }
    }
    for (const Widget& widget : widgets) {
        const Rect& rect   = widget.rect;
        const f32   centre = rect.x + rect.w * 0.5F;
        const f32   text_y = rect.y + std::floor((rect.h - 8.0F) * 0.5F);
        switch (widget.kind) {
            case WidgetKind::Button:
                gui.text_centred(centre, text_y, widget.text,
                                 widget.active ? widget.colour : 0xFFA0A0A0U);
                break;
            case WidgetKind::Slider:
                gui.text_centred(centre, text_y, widget.text, 0xFFFFFFFFU);
                break;
            case WidgetKind::EditBox: {
                const f32 end = gui.text(rect.x + 4.0F, text_y, widget.text, 0xFFE0E0E0U);
                if (widget.text.empty() && !widget.hint.empty()) {
                    gui.text(rect.x + 4.0F, text_y, widget.hint, 0xFF808080U);
                }
                if (widget.selected && cursor_visible) {
                    gui.text(end, text_y, "_", 0xFFE0E0E0U);
                }
                break;
            }
            case WidgetKind::Label:
                if (widget.left_aligned) {
                    gui.text(rect.x, rect.y, widget.text, widget.colour);
                } else {
                    gui.text_centred(centre, rect.y, widget.text, widget.colour);
                }
                break;
            case WidgetKind::Tab:
                gui.text_centred(centre, text_y, widget.text,
                                 widget.selected ? 0xFFFFFFFFU : 0xFFA0A0A0U);
                break;
            case WidgetKind::Icon: break;  // the owner draws the image
        }
    }
}

}  // namespace ov::client
