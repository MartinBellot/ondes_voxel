// One decoded texture, by resource location.
//
// The atlas has its own loader because it needs to scale, mip and stitch. The
// interface does not: a widget sheet, a container background and a font page
// are read once, uploaded whole, and addressed in their own pixels. Scaling
// one of those into an atlas would throw away the very thing they are indexed
// by — `widgets.png` is a 256×256 layout in which the hotbar starts at (0, 0)
// and is 182 wide, and every one of those numbers is a fact about the file.
//
// So this returns the file, and the caller keeps the pixel arithmetic.
#pragma once

#include "ov/base/resource_location.hpp"
#include "ov/base/types.hpp"
#include "ov/render/asset_source.hpp"

#include <expected>
#include <string_view>
#include <vector>

namespace ov::render {

/// RGBA8, row-major from the top-left, always width * height * 4 bytes.
struct TextureImage {
    u32             width{0};
    u32             height{0};
    std::vector<u8> rgba;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }

    [[nodiscard]] usize index(u32 x, u32 y) const noexcept {
        return (static_cast<usize>(y) * width + x) * 4;
    }
};

enum class TextureError : u8 {
    /// No such file anywhere in the pack stack.
    NotFound,
    /// The file is there and is not a PNG this decoder can read.
    Malformed,
};

[[nodiscard]] std::string_view to_string(TextureError error) noexcept;

/// `minecraft:gui/widgets` -> `assets/minecraft/textures/gui/widgets.png`.
[[nodiscard]] std::expected<TextureImage, TextureError> load_texture(
    const AssetSource& source, const ResourceLocation& location);

/// The same, by pack-relative path.
///
/// The font format needs it: a `bitmap` provider's `file` is
/// `minecraft:font/ascii.png` — extension included, unlike every texture a
/// model names — so appending `.png` to it would look for `ascii.png.png`.
[[nodiscard]] std::expected<TextureImage, TextureError> load_texture_path(
    const AssetSource& source, std::string_view path);

}  // namespace ov::render
