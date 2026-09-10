// The interface's one drawing primitive: a textured, tinted quad in screen
// pixels.
//
// Everything above this — the hotbar, the hearts, the inventory, the text, the
// item models — is a list of quads. That is not a simplification of what
// vanilla does, it is what vanilla does: its whole GUI is `blit` calls into a
// sprite sheet, and the sheets are in the resource pack rather than in the
// code.
//
// Three things are worth knowing before reading the rest:
//
//   • **GUI pixels, not screen pixels.** The interface is laid out in a
//     virtual 320×240-and-up space and multiplied by an integer scale, which
//     is why Minecraft's buttons are the same size on a laptop and on a 4K
//     display. Everything passed to this class is in GUI pixels; the scale is
//     applied when the vertex is written. See auto_gui_scale().
//
//   • **Colours are sRGB, and the swapchain is too.** A tint given here is
//     0xAARRGGBB exactly as vanilla writes it, and it is converted to linear
//     before it reaches the vertex, so that a white glyph tinted 0xFF5555
//     lands on the screen as 0xFF5555 and not as the darker value a linear
//     multiply would give. Alpha is not converted: it never was gamma-encoded.
//
//   • **One vertex buffer, one pipeline, a draw per texture change.** Batches
//     are cut only when the bound image changes, so a screen that draws its
//     background, then its slots, then its items, then its text costs four or
//     five draws no matter how many quads are in it. Nothing here allocates
//     once the reserves are warm: the vertex scratch is a member.
#pragma once

#include "ov/base/types.hpp"
#include "ov/render/font.hpp"
#include "ov/render/texture_image.hpp"
#include "ov/rhi/device.hpp"

#include <array>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace ov::client {

/// An image the GUI can draw from. Not a handle: an index into the table the
/// Gui owns, so that a caller never touches an rhi type to draw a widget.
enum class GuiTexture : u16 { Invalid = 0xFFFF };

/// A point in GUI pixels.
struct GuiPoint {
    f32 x{0.0F};
    f32 y{0.0F};
};

/// What one frame's interface cost, in objects.
struct GuiStats {
    u32 quads{0};
    u32 draws{0};
    u32 vertices{0};
    /// The high-water mark since creation, so a buffer that is nearly full is
    /// visible before it overflows.
    u32 peak_vertices{0};
};

/// Vanilla's automatic GUI scale: the largest integer that still leaves at
/// least 320×240 GUI pixels on screen, and at least one.
///
/// The rule is what makes a HUD readable at 4K and still fit on a 640×480
/// window. `maximum` is the player's setting; zero means automatic.
[[nodiscard]] u32 auto_gui_scale(u32 width, u32 height, u32 maximum = 0) noexcept;

class Gui {
public:
    /// The vertex ring, per frame in flight. Twelve thousand quads: the
    /// creative inventory is the biggest screen vanilla has and it draws about
    /// two thousand.
    static constexpr u32 kMaxQuads = 12288;

    [[nodiscard]] static std::expected<std::unique_ptr<Gui>, rhi::RhiError> create(
        rhi::Device& device, rhi::Format colour_format);

    Gui(const Gui&)            = delete;
    Gui& operator=(const Gui&) = delete;
    ~Gui();

    // ── Setup, once ─────────────────────────────────────────────────────────

    /// Upload an image and keep it for the life of the Gui.
    [[nodiscard]] std::expected<GuiTexture, rhi::RhiError> add_texture(
        const render::TextureImage& image, std::string_view name);

    /// Reference an image somebody else owns — the block atlas, above all,
    /// which the item renderer draws from and which must not be uploaded
    /// twice.
    [[nodiscard]] GuiTexture borrow_texture(rhi::ImageHandle image, u32 width, u32 height);

    /// Take the font and upload its pages. Must be called before any text.
    [[nodiscard]] std::expected<void, rhi::RhiError> set_font(render::Font font);

    [[nodiscard]] const render::Font& font() const noexcept { return font_; }

    [[nodiscard]] bool has_font() const noexcept { return !font_.pages().empty(); }

    // ── Per frame ───────────────────────────────────────────────────────────

    /// Start a frame. `scale` is the GUI scale; the width and height are the
    /// framebuffer's, in real pixels.
    void begin(u32 framebuffer_width, u32 framebuffer_height, u32 scale);

    /// The interface's own size, in GUI pixels.
    [[nodiscard]] f32 width() const noexcept { return width_; }
    [[nodiscard]] f32 height() const noexcept { return height_; }
    [[nodiscard]] u32 scale() const noexcept { return scale_; }

    /// A quad, with explicit texture coordinates in 0..1.
    void quad(GuiTexture texture, f32 x, f32 y, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1,
              u32 argb = 0xFFFFFFFFU);

    /// A quad whose four corners are given, for anything rotated — which in
    /// practice means the faces of a block model in its GUI cell.
    void quad_corners(GuiTexture texture, const std::array<GuiPoint, 4>& corners,
                      const std::array<GuiPoint, 4>& uvs, u32 argb);

    /// A rect from a sprite sheet, addressed in the sheet's own pixels.
    ///
    /// `sheet_width` and `sheet_height` are the sheet's *logical* size — 256
    /// for `widgets.png`, whatever the resolution of the pack that redrew it.
    /// That indirection is the whole reason this exists: Faithful's widgets
    /// sheet is 512 pixels across and every coordinate in vanilla's layout is
    /// still a number out of 256.
    void blit(GuiTexture texture, f32 x, f32 y, f32 w, f32 h, f32 sx, f32 sy, f32 sw, f32 sh,
              f32 sheet_width = 256.0F, f32 sheet_height = 256.0F, u32 argb = 0xFFFFFFFFU);

    /// A solid rectangle.
    void fill(f32 x, f32 y, f32 w, f32 h, u32 argb);

    /// Vanilla's vertical gradient, which is exactly what the dim behind an
    /// open screen is.
    void gradient(f32 x, f32 y, f32 w, f32 h, u32 top_argb, u32 bottom_argb);

    /// Draw a line of text and return the pen position after it.
    ///
    /// `§` codes are obeyed. The shadow is vanilla's: the same text offset one
    /// GUI pixel right and down, in the colour with every channel divided by
    /// four, drawn first so the glyphs land on top of it.
    f32 text(f32 x, f32 y, std::string_view utf8, u32 argb = 0xFFFFFFFFU, bool shadow = true);

    /// The same at `size` times the font's size: a title is drawn at 4, its
    /// subtitle at 2. Everything scales — advance, shadow offset, bold step,
    /// underline — so a size-4 glyph is exactly a size-1 glyph magnified.
    f32 text_scaled(f32 x, f32 y, std::string_view utf8, u32 argb, f32 size, bool shadow = true);

    /// The same, centred on `x`.
    f32 text_centred(f32 x, f32 y, std::string_view utf8, u32 argb = 0xFFFFFFFFU,
                     bool shadow = true);

    /// The same, ending at `x`.
    f32 text_right(f32 x, f32 y, std::string_view utf8, u32 argb = 0xFFFFFFFFU,
                   bool shadow = true);

    /// Record every batch collected since begin(). Inside a render pass.
    void flush(rhi::CommandList& cmd);

    [[nodiscard]] const GuiStats& stats() const noexcept { return stats_; }

private:
    struct Vertex {
        f32 x{0.0F};
        f32 y{0.0F};
        f32 u{0.0F};
        f32 v{0.0F};
        /// RGBA8, linear, premultiplied by nothing.
        std::array<u8, 4> colour{};
    };

    struct Batch {
        rhi::ImageHandle image;
        u32              first{0};
        u32              count{0};
    };

    struct Texture {
        rhi::ImageHandle image;
        u32              width{0};
        u32              height{0};
        /// False for a borrowed image, which somebody else destroys.
        bool owned{false};
    };

    Gui() = default;

    void push_quad(GuiTexture texture, const std::array<GuiPoint, 4>& corners,
                   const std::array<GuiPoint, 4>& uvs, u32 argb);

    [[nodiscard]] const Texture* lookup(GuiTexture texture) const noexcept;

    rhi::Device*        device_{nullptr};
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle  sampler_;
    /// One texel, opaque white: what a solid fill samples.
    GuiTexture white_{GuiTexture::Invalid};

    std::vector<Texture>           textures_;
    std::vector<rhi::BufferHandle> vertices_;
    u32                            ring_{0};

    render::Font            font_;
    std::vector<GuiTexture> font_pages_;

    std::vector<Vertex> scratch_;
    std::vector<Batch>  batches_;

    /// The size text() draws at; 1 except inside text_scaled().
    f32 text_size_{1.0F};

    f32      width_{0.0F};
    f32      height_{0.0F};
    /// The framebuffer in real pixels, which is what the projection divides
    /// by — not width_ × scale_, which is larger when the size does not divide.
    f32      framebuffer_width_{1.0F};
    f32      framebuffer_height_{1.0F};
    u32      scale_{1};
    GuiStats stats_;
};

}  // namespace ov::client
