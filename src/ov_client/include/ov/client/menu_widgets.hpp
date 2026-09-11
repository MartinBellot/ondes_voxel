// The widgets of vanilla's menus: the button, the slider, the text box, the
// dirt behind them, and the rule that lays out a column of them.
//
// Every one is drawn from the pack's own sheets — `gui/widgets.png` for the
// 200×20 button in its three states, `gui/slider.png` for the slider track
// and handle, `gui/options_background.png` for the dirt — so a resource pack
// that redraws them is obeyed, exactly as vanilla obeys it. Positions and
// sizes are GUI pixels; the numbers were read off the running 1.20.1 client
// (scripts/measure_screens.py, docs/provenance/ecrans.md).
//
// No state lives here. A screen is a list of Widget values, laid out by a
// function of the GUI size, and hit-tested by the same rectangles it is drawn
// with — which is what makes the layout testable without a device.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/gui.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

struct MenuTextures {
    /// `gui/widgets.png`: the button at v = 46 (disabled), 66, 86 (hovered).
    GuiTexture widgets{GuiTexture::Invalid};
    /// `gui/slider.png`: the track at v = 0 / 20 (focused), the handle at
    /// v = 40 / 60 (hovered), each 200×20.
    GuiTexture slider{GuiTexture::Invalid};
    /// `gui/options_background.png`: tiled every 32 GUI pixels.
    GuiTexture dirt{GuiTexture::Invalid};
    /// `gui/title/minecraft.png` and `edition.png`, for the title screen.
    GuiTexture logo{GuiTexture::Invalid};
    GuiTexture edition{GuiTexture::Invalid};
    /// `gui/title/background/panorama_0..5.png`: the cube behind the title.
    std::array<GuiTexture, 6> panorama{GuiTexture::Invalid, GuiTexture::Invalid,
                                       GuiTexture::Invalid, GuiTexture::Invalid,
                                       GuiTexture::Invalid, GuiTexture::Invalid};
    /// `gui/tab_button.png`: the Create World tabs.
    GuiTexture tab{GuiTexture::Invalid};
    /// `gui/accessibility.png`: a 32×64 sheet, the 20×20 icon at v = 0 and
    /// its hovered form at v = 20.
    GuiTexture accessibility{GuiTexture::Invalid};
    /// `gui/header_separator.png` and `footer_separator.png`.
    GuiTexture header_separator{GuiTexture::Invalid};
    GuiTexture footer_separator{GuiTexture::Invalid};
};

struct Rect {
    f32 x{0.0F};
    f32 y{0.0F};
    f32 w{0.0F};
    f32 h{0.0F};

    [[nodiscard]] bool contains(f32 px, f32 py) const noexcept {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

enum class WidgetKind : u8 {
    Button,
    /// A slider: `value` is 0..1, the label is drawn over it.
    Slider,
    /// A bordered text box; the text is the owner's.
    EditBox,
    /// A line of text, centred on the rectangle.
    Label,
    /// A Create World tab.
    Tab,
    /// A 20×20 image button (the title's language and accessibility icons):
    /// hit like a button, drawn by its owner.
    Icon,
};

/// One widget of a screen. `id` is what the screen's owner acts on when it is
/// clicked; `text` is already translated.
struct Widget {
    std::string id;
    WidgetKind  kind{WidgetKind::Button};
    Rect        rect;
    std::string text;
    bool        active{true};
    f64         value{0.0};
    /// A tab that is the current one, or a key binding waiting for a key.
    bool        selected{false};
    u32         colour{0xFFFFFFFFU};
    /// A Label drawn from rect.x instead of centred on the rectangle.
    bool        left_aligned{false};
    /// An EditBox's grey placeholder while it is empty ("Leave blank for a
    /// random seed"), already translated.
    std::string hint;
};

/// The widget under a point, or null. Inactive widgets are still hit (they
/// swallow the click, as vanilla's do) but the caller should not act.
[[nodiscard]] const Widget* widget_at(std::span<const Widget> widgets, f32 x, f32 y) noexcept;
[[nodiscard]] Widget*       find_widget(std::span<Widget> widgets, std::string_view id) noexcept;

/// The 200×20 sheet button, stretched to any width by drawing its left and
/// right halves, with its label centred 6 pixels down: white, or `0xA0A0A0`
/// when inactive.
void draw_button(Gui& gui, const MenuTextures& textures, const Rect& rect, std::string_view text,
                 bool active, bool hovered, u32 colour = 0xFFFFFFFFU);

/// A slider: the track, the 8-pixel handle at `value`, and the label.
void draw_slider(Gui& gui, const MenuTextures& textures, const Rect& rect, std::string_view text,
                 f64 value, bool hovered);

/// Vanilla's bordered text box: a one-pixel frame (`0xFFA0A0A0`, white when
/// focused) around black, the text 4 pixels in, `0xE0E0E0`.
void draw_edit_box(Gui& gui, const Rect& rect, std::string_view text, bool focused,
                   bool cursor_visible);

/// The value a slider takes for a pointer at `x`: the handle's centre follows
/// the pointer, clamped to the track.
[[nodiscard]] f64 slider_value_at(const Rect& rect, f32 x) noexcept;

/// The dirt: `options_background.png` tiled every 32 GUI pixels over the
/// rectangle, tinted `tint` (vanilla's menus use 0xFF404040).
void draw_dirt(Gui& gui, const MenuTextures& textures, const Rect& rect,
               u32 tint = 0xFF404040U);

/// Every widget of a screen, with `hovered` lit.
void draw_widgets(Gui& gui, const MenuTextures& textures, std::span<const Widget> widgets,
                  f32 mouse_x, f32 mouse_y, bool cursor_visible);

}  // namespace ov::client
